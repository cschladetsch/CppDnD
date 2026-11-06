#include "dnd/Llm.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

#include "dnd/AI.hpp"
#include "dnd/Util.hpp"
#include "httplib.h"

#ifdef DND_HAVE_MODELSTORE
#include "DeepSeekStreamParser.hpp"
#include "ModelStore.hpp"
#endif

namespace dnd {
namespace {

std::string Env(const char* k, const std::string& def = "") {
    const char* v = std::getenv(k);
    return v && *v ? v : def;
}

std::string Trim(std::string s) {
    const auto issp = [](unsigned char c) { return std::isspace(c) != 0; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [&](char c) { return !issp(static_cast<unsigned char>(c)); }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [&](char c) { return !issp(static_cast<unsigned char>(c)); }).base(), s.end());
    return s;
}

}  // namespace

// ---------------------------------------------------------------- model store

ModelInfo LocateModel(const std::string& name) {
    ModelInfo m;
    m.name = name;
#ifdef DND_HAVE_MODELSTORE
    m.home = deepseek::ModelStore::ResolveModelHome();
    m.path = deepseek::ModelStore::ResolveModelPath(name);
    m.exists = !name.empty() && deepseek::ModelStore::ModelExists(name);
#else
    const std::string home = Env("DEEPSEEK_MODEL_HOME", Env("HOME", ".") + "/.local/share/deepseek/models");
    m.home = home;
    m.path = home + "/" + name;
    std::error_code ec;
    m.exists = !name.empty() && std::filesystem::exists(m.path, ec);
#endif
    if (m.exists) {
        std::error_code ec;
        for (const auto& e : std::filesystem::recursive_directory_iterator(m.path, std::filesystem::directory_options::skip_permission_denied, ec))
            if (e.is_regular_file(ec) && e.path().extension() == ".gguf") { m.weights = e.path().string(); break; }
    }
    return m;
}

// ---------------------------------------------------------------- text helpers

std::string StripThink(std::string s) {
    for (;;) {
        const auto a = s.find("<think>");
        if (a == std::string::npos) break;
        const auto b = s.find("</think>", a);
        s.erase(a, b == std::string::npos ? std::string::npos : b + 8 - a);
    }
    if (const auto b = s.find("</think>"); b != std::string::npos) s.erase(0, b + 8);  // template opened <think> for us
    return Trim(s);
}

std::string ExtractJsonObject(const std::string& s) {
    int depth = 0;
    bool inStr = false, esc = false;
    std::size_t start = std::string::npos;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (inStr) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') inStr = true;
        else if (c == '{') { if (depth++ == 0) start = i; }
        else if (c == '}' && depth > 0 && --depth == 0) return s.substr(start, i - start + 1);
    }
    return {};
}

// ---------------------------------------------------------------- client

LlmConfig LlmConfig::FromEnv() {
    LlmConfig c;
    c.baseUrl = Env("DND_LLM_URL", c.baseUrl);
    c.path = Env("DND_LLM_PATH", c.path);
    c.model = Env("DND_LLM_MODEL");
    c.apiKey = Env("DND_LLM_KEY");
    if (const auto t = Env("DND_LLM_TIMEOUT"); !t.empty()) c.timeoutSec = std::max(1, std::atoi(t.c_str()));
    c.stream = Env("DND_LLM_STREAM", "1") != "0";
    return c;
}

LlmReply HttpLlmClient::Chat(const std::string& system, const std::string& user, bool jsonMode) {
    LlmReply out;
    nlohmann::json body{{"model", cfg_.model},
                        {"messages", nlohmann::json::array({{{"role", "system"}, {"content", system}}, {{"role", "user"}, {"content", user}}})},
                        {"temperature", cfg_.temperature},
                        {"max_tokens", cfg_.maxTokens},
                        {"stream", cfg_.stream}};
    if (jsonMode) body["response_format"] = {{"type", "json_object"}};

    try {
        httplib::Client cli(cfg_.baseUrl);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(cfg_.timeoutSec);
        cli.set_write_timeout(10);
        httplib::Headers headers;
        if (!cfg_.apiKey.empty()) headers.emplace("Authorization", "Bearer " + cfg_.apiKey);

        std::string raw;
        std::string parseError;
#ifdef DND_HAVE_MODELSTORE
        deepseek::DeepSeekStreamParser parser([&](std::string_view reasoning, std::string_view content) {
            out.reasoning.append(reasoning);
            out.content.append(content);
        });
#endif
        httplib::Request req;
        req.method = "POST";
        req.path = cfg_.path;
        req.headers = headers;
        req.set_header("Content-Type", "application/json");
        req.body = body.dump();
        if (cfg_.stream) {
            req.content_receiver = [&](const char* data, size_t len, uint64_t, uint64_t) {
                raw.append(data, len);
#ifdef DND_HAVE_MODELSTORE
                return parser.Feed(std::string_view(data, len), &parseError);
#else
                return true;
#endif
            };
        }
        auto res = cli.send(req);
        if (!res) {
            out.error = "LLM request failed: " + httplib::to_string(res.error());
            return out;
        }
        if (res->status < 200 || res->status >= 300) {
            out.error = Cat("LLM HTTP ", res->status);
            return out;
        }
        if (!parseError.empty()) { out.error = parseError; return out; }
        if (!cfg_.stream) {
            raw = res->body;
        }
        if (!cfg_.stream || out.content.empty()) {
            // Non-streamed JSON body (or a server that ignored stream=true).
            const auto j = nlohmann::json::parse(raw, nullptr, false);
            if (!j.is_discarded() && j.contains("choices") && !j["choices"].empty()) {
                const auto& msg = j["choices"][0].value("message", nlohmann::json::object());
                if (msg.contains("content") && msg["content"].is_string()) out.content = msg["content"].get<std::string>();
                if (msg.contains("reasoning_content") && msg["reasoning_content"].is_string()) out.reasoning = msg["reasoning_content"].get<std::string>();
            }
        }
        out.content = StripThink(out.content);
        out.ok = !out.content.empty();
        if (!out.ok) out.error = "LLM returned no content";
    } catch (const std::exception& e) {
        out.error = std::string("LLM client error: ") + e.what();
    }
    return out;
}

LlmReply MockLlmClient::Chat(const std::string& system, const std::string& user, bool jsonMode) {
    ++calls_;
    lastUser_ = user;
    if (responder_) return responder_(system, user, jsonMode);
    LlmReply r;
    r.ok = true;
    r.content = jsonMode ? R"({"actions":[{"type":"end_turn"}]})" : "The air tightens and the shadows lean in.";
    return r;
}

// ---------------------------------------------------------------- narrator

Game::Narrator MakeLlmNarrator(std::shared_ptr<ILlmClient> client, const Game& game) {
    struct State { int failures = 0; int skip = 0; };
    auto st = std::make_shared<State>();
    return [client, &game, st](const Event& e) -> std::string {
        if (st->skip > 0) { --st->skip; return {}; }
        const char* what = nullptr;
        switch (e.type) {
            case EventType::Scene: what = "a scene"; break;
            case EventType::Down: what = "a hero falling"; break;
            case EventType::Death: what = "a death"; break;
            case EventType::CombatEnd: what = "the end of a fight"; break;
            case EventType::Quest: what = "a quest being completed"; break;
            default: return {};
        }
        const std::string system =
            "You are the narrator of a gritty gothic-horror Dungeons & Dragons 5e game. Reply with one or two vivid sentences of "
            "present-tense prose. Never state dice rolls, numbers or rules. Never invent new named characters, items or outcomes "
            "that the event does not state. Do not repeat the event text word for word.";
        const std::string user = Cat("Setting: ", game.Module().setting, "\nLocation: ", game.LocationId(), "\nTime: ",
                                     game.Module().IsNight(game.ClockHour()) ? "night" : "day", "\nThis is ", what, ":\n", e.text);
        const LlmReply r = client->Chat(system, user, false);
        if (!r.ok) {
            if (++st->failures >= 3) { st->failures = 0; st->skip = 20; }
            return {};
        }
        st->failures = 0;
        std::string t = r.content;
        if (t.size() > 600) t.resize(600);
        std::replace(t.begin(), t.end(), '\n', ' ');
        return t;
    };
}

// ---------------------------------------------------------------- LLM DM

LlmDmController::LlmDmController(std::shared_ptr<ILlmClient> client) : client_(std::move(client)), fallback_(std::make_unique<AIController>()) {}

std::string LlmDmController::BuildPrompt(const TurnContext& ctx) {
    const GameActor& me = ctx.self;
    const auto& s = me.sheet;
    std::string p = Cat("You control ", me.Name(), " (id ", me.id, ") at (", me.pos.x, ",", me.pos.y, "), HP ", s.hp, "/", s.maxHp, ", AC ", me.EffectiveAc(),
                        ", movement ", s.economy.movementFt, " ft (5 ft per square). Round ", ctx.combat.Round(), ".\nYour attacks:\n");
    for (std::size_t i = 0; i < s.attacks.size(); ++i)
        p += Cat("  ", i, ": ", s.attacks[i].name, " reach/range ", s.attacks[i].rangeFt, " ft\n");
    for (const auto& id : s.spells) p += Cat("  spell ", id, "\n");
    p += "Map (x right, y down; # wall, c/O cover, ~ rough):\n";
    const auto& g = ctx.combat.Grid();
    for (int y = 0; y < g.Height(); ++y) {
        std::string row;
        for (int x = 0; x < g.Width(); ++x) {
            const int occ = g.OccupantAt({x, y});
            if (occ >= 0) row += static_cast<char>('0' + (occ % 10));
            else row += g.At({x, y}) == Terrain::Wall ? '#' : g.At({x, y}) == Terrain::HalfCover ? 'c' : g.At({x, y}) == Terrain::ThreeQuarterCover ? 'O' : g.At({x, y}) == Terrain::Difficult ? '~' : '.';
        }
        p += Cat("  ", row, "\n");
    }
    p += "Creatures (digit on the map = id mod 10):\n";
    for (const auto& a : ctx.combat.Actors()) {
        if (a->IsDead()) continue;
        p += Cat("  id ", a->id, " ", a->Name(), a->team == me.team ? " [ally]" : " [enemy]", " at (", a->pos.x, ",", a->pos.y, ") HP ", a->sheet.hp, "/", a->sheet.maxHp,
                 " AC ", a->EffectiveAc(), a->IsDown() ? " DOWN" : "", "\n");
    }
    p += "Plan this one turn. Reply with JSON only:\n"
         "{\"actions\":[{\"type\":\"move\",\"dest\":[x,y]},{\"type\":\"attack\",\"target\":ID,\"attack\":INDEX},{\"type\":\"end_turn\"}]}\n"
         "Types: move, attack, cast (needs \"spell\" and \"target\"), dash, dodge, end_turn. At most 4 actions; melee needs adjacency.";
    return p;
}

std::vector<Action> LlmDmController::ParsePlan(const std::string& text, std::string& err) {
    std::vector<Action> plan;
    const std::string obj = ExtractJsonObject(text);
    if (obj.empty()) { err = "no JSON object in reply"; return plan; }
    const auto j = nlohmann::json::parse(obj, nullptr, false);
    if (j.is_discarded() || !j.contains("actions") || !j["actions"].is_array()) { err = "reply has no \"actions\" array"; return plan; }
    for (const auto& a : j["actions"]) {
        Action act;
        std::string e;
        if (!ActionFromJson(a, act, e)) { err = e; plan.clear(); return plan; }
        plan.push_back(act);
        if (plan.size() >= 4) break;
    }
    if (plan.empty()) err = "empty plan";
    return plan;
}

Action LlmDmController::DecideAction(const TurnContext& ctx) {
    if (ctx.combat.Round() != planRound_ || ctx.self.id != planActor_) {
        planRound_ = ctx.combat.Round();
        planActor_ = ctx.self.id;
        plan_.clear();
        useFallback_ = false;
        const LlmReply r = client_->Chat(
            "You are the Dungeon Master of a D&D 5e game, playing one monster's turn. Choose rules-legal, tactically sound actions. "
            "Output only the JSON object requested.",
            BuildPrompt(ctx), true);
        std::string err;
        if (r.ok) plan_ = ParsePlan(r.content, err);
        if (!r.ok || plan_.empty()) useFallback_ = true;
        std::reverse(plan_.begin(), plan_.end());  // pop from the back
    }
    if (useFallback_) return fallback_->DecideAction(ctx);
    if (plan_.empty()) return Action::End();
    Action a = plan_.back();
    plan_.pop_back();
    return a;
}

void LlmDmController::OnActionResult(const TurnContext& ctx, const Action&, bool ok, const std::string&) {
    if (ok || useFallback_) return;
    plan_.clear();
    useFallback_ = true;  // the rest of this turn is played by the built-in AI
    (void)ctx;
}

}  // namespace dnd
