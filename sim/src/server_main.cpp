// dnd_server: HTTP/JSON backend for a web front end. See docs/API.md.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <regex>
#include <vector>

#include <filesystem>

#include "dnd/Agents.hpp"
#include "dnd/Content.hpp"
#include "dnd/Game.hpp"
#include "dnd/Images.hpp"
#include "dnd/Llm.hpp"
#include "dnd/Scene.hpp"
#include "httplib.h"

using namespace dnd;
using Json = nlohmann::json;

namespace {

struct Slot {
    std::mutex m;
    std::unique_ptr<Game> game;
    std::shared_ptr<AgentTable> agents;  // set when the game was created with "agents": true
    std::string name;                    // the session's name, shown in the web client's dropdown
    long long created = 0, updated = 0;  // seconds since the epoch
};

struct Server {
    std::mutex mapMutex;
    std::map<std::string, std::shared_ptr<Slot>> games;
    std::shared_ptr<ILlmClient> llm;  // null: LLM features disabled
    LlmConfig llmCfg;
    bool mock = false;
    std::optional<AgentsConfig> agentsCfg;  // from --agents / DND_AGENTS / <data>/agents.json
    std::string agentsPath, agentsError;
    std::string sessionsDir;  // every game is written here as <id>.json after each change and restored at startup; empty: sessions live in memory only
    std::mt19937_64 idRng{std::random_device{}()};

    std::string NewId() {
        static const char* hex = "0123456789abcdef";
        std::string s;
        for (int i = 0; i < 12; ++i) s.push_back(hex[idRng() % 16]);
        return s;
    }
};

struct HttpError {
    int status;
    std::string message;
};

void Reply(httplib::Response& res, int status, const Json& j) {
    res.status = status;
    res.set_content(j.dump(), "application/json");
}

Json Body(const httplib::Request& req) {
    if (req.body.empty()) return Json::object();
    Json j = Json::parse(req.body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) throw HttpError{400, "request body must be a JSON object"};
    return j;
}

// Attaches whatever model features the game asks for. Returns the agent table when the game uses agents.
std::shared_ptr<AgentTable> Wire(Server& s, Game& g) {
    const GameConfig& c = g.Config();
    if (c.agents) {
        if (!s.agentsCfg) throw HttpError{400, s.agentsError.empty() ? "agents were requested but there is no agents file (use --agents, DND_AGENTS, or data/agents.json)" : s.agentsError};
        AgentTable::ClientFactory factory;
        if (s.mock) factory = [](const std::string&, const BackendDef&, const AgentDef&) { return std::make_shared<MockLlmClient>(); };
        auto table = std::make_shared<AgentTable>(*s.agentsCfg, factory);
        AgentTable::Attach(table, g);
        return table;
    }
    if (!c.llmDm && !c.llmNarration) return nullptr;
    if (!s.llm) throw HttpError{400, "LLM features were requested but no model is configured (set DND_LLM_MODEL / DND_LLM_URL, or run with --mock-llm)"};
    if (c.llmNarration) g.SetNarrator(MakeLlmNarrator(s.llm, g));
    if (c.llmDm) {
        auto client = s.llm;
        g.SetMonsterFactory([client] { return std::make_unique<LlmDmController>(client); });
    }
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    std::string dataDir = Content::DefaultDataDir();
    std::string staticDir, artDir, histDir, imagesPath, host = "127.0.0.1";
#ifdef DND_HISTORY_DIR
    histDir = DND_HISTORY_DIR;  // x-y.json and group-history.json per game, in <dir>/<game id>/; --history overrides, --history "" turns files off
#endif
    if (const char* h = std::getenv("DND_HISTORY"); h) histDir = h;
    std::string sessionsDir;
#ifdef DND_SESSIONS_DIR
    sessionsDir = DND_SESSIONS_DIR;  // sessions: whole games, kept across restarts; --sessions overrides, --sessions "" keeps them in memory only
#endif
    if (const char* d = std::getenv("DND_SESSIONS"); d) sessionsDir = d;
#ifdef DND_ART_DIR
    artDir = DND_ART_DIR;  // generated or hand-made pictures; --art overrides
#endif
    if (const char* a = std::getenv("DND_ART"); a && *a) artDir = a;
#ifdef DND_WEB_DIR
    staticDir = DND_WEB_DIR;  // the bundled web client; --static overrides, --static "" disables
#endif
    if (const char* w = std::getenv("DND_WEB"); w && *w) staticDir = w;
    int port = 8088;
    Server S;
    S.llmCfg = LlmConfig::FromEnv();

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--port") port = std::atoi(next().c_str());
        else if (a == "--host") host = next();
        else if (a == "--data") dataDir = next();
        else if (a == "--static") staticDir = next();
        else if (a == "--art") artDir = next();
        else if (a == "--history") histDir = next();
        else if (a == "--sessions") sessionsDir = next();
        else if (a == "--images") imagesPath = next();
        else if (a == "--llm-url") S.llmCfg.baseUrl = next();
        else if (a == "--llm-model") S.llmCfg.model = next();
        else if (a == "--mock-llm") S.mock = true;
        else if (a == "--agents") S.agentsPath = next();
        else if (a == "--ollama") { S.llmCfg.baseUrl = "http://127.0.0.1:11434"; S.llmCfg.model = next(); S.llmCfg.stream = false; }
        else if (a == "--help" || a == "-h") {
            std::cout << "dnd_server [--port 8088] [--host 127.0.0.1] [--data DIR] [--static DIR]\n"
                         "           [--llm-url http://127.0.0.1:8080] [--llm-model NAME] [--ollama MODEL] [--agents FILE] [--art DIR] [--images FILE] [--history DIR] [--sessions DIR] [--mock-llm]\n"
                         "LLM env: DND_LLM_URL DND_LLM_PATH DND_LLM_MODEL DND_LLM_KEY DND_LLM_TIMEOUT DND_LLM_STREAM\n"
                         "Models are looked up in the shared model store (DEEPSEEK_MODEL_HOME).\n";
            return 0;
        }
    }

    try {
        Content::Load(dataDir);
    } catch (const std::exception& e) {
        std::cerr << "failed to load content from '" << dataDir << "': " << e.what() << "\n";
        return 1;
    }
    S.sessionsDir = sessionsDir;
    if (S.mock) S.llm = std::make_shared<MockLlmClient>();
    else if (!S.llmCfg.model.empty() || std::getenv("DND_LLM_URL")) S.llm = std::make_shared<HttpLlmClient>(S.llmCfg);

    {   // the global agents file: explicit path must load; the default one is optional
        namespace fs = std::filesystem;
        const bool explicitPath = !S.agentsPath.empty() || (std::getenv("DND_AGENTS") && *std::getenv("DND_AGENTS"));
        if (S.agentsPath.empty()) S.agentsPath = std::getenv("DND_AGENTS") && *std::getenv("DND_AGENTS") ? std::getenv("DND_AGENTS") : (fs::path(dataDir) / "agents.json").string();
        if (explicitPath || fs::exists(S.agentsPath)) {
            std::string err;
            S.agentsCfg = AgentsConfig::Load(S.agentsPath, err);
            if (!S.agentsCfg) {
                S.agentsError = err;
                if (explicitPath) { std::cerr << err << "\n"; return 2; }
                std::cerr << "warning: " << err << " (agents disabled)\n";
            }
        }
    }

    httplib::Server svr;
    svr.set_default_headers({{"Access-Control-Allow-Origin", "*"}, {"Access-Control-Allow-Headers", "Content-Type"}, {"Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS"}});
    svr.Options(R"(.*)", [](const httplib::Request&, httplib::Response& res) { res.status = 204; });
    svr.set_payload_max_length(8 * 1024 * 1024);
    svr.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        try { std::rethrow_exception(ep); }
        catch (const std::exception& e) { Reply(res, 500, {{"error", e.what()}}); }
        catch (...) { Reply(res, 500, {{"error", "unknown error"}}); }
    });

    // Wraps a handler: JSON errors, unknown ids and exceptions all become structured replies.
    auto guarded = [&](auto fn) {
        return [fn](const httplib::Request& req, httplib::Response& res) {
            try { fn(req, res); }
            catch (const HttpError& e) { Reply(res, e.status, {{"error", e.message}}); }
            catch (const Json::exception& e) { Reply(res, 400, {{"error", std::string("bad request: ") + e.what()}}); }
            catch (const std::exception& e) { Reply(res, 500, {{"error", e.what()}}); }
        };
    };
    auto find = [&](const httplib::Request& req) {
        std::lock_guard<std::mutex> lk(S.mapMutex);
        auto it = S.games.find(req.path_params.at("id"));
        if (it == S.games.end()) throw HttpError{404, "no such game"};
        return it->second;
    };
    auto sinceOf = [](const httplib::Request& req) { return req.has_param("since") ? std::atoi(req.get_param_value("since").c_str()) : 0; };
    auto nowSec = [] { return static_cast<long long>(std::time(nullptr)); };
    // Adds a game as a session. Without a name it is called "<module name> #n", the first n not in use.
    auto add = [&](std::unique_ptr<Game> g, std::string name = "", std::string id = "", long long created = 0) {
        std::lock_guard<std::mutex> lk(S.mapMutex);
        if (S.games.size() >= 256) throw HttpError{429, "too many games; delete some first"};
        auto slot = std::make_shared<Slot>();
        if (name.empty()) {
            const std::string base = g->Module().name.empty() ? std::string("Game") : g->Module().name;
            for (int n = 1;; ++n) {
                name = base + " #" + std::to_string(n);
                bool used = false;
                for (const auto& kv : S.games) used = used || kv.second->name == name;
                if (!used) break;
            }
        }
        slot->game = std::move(g);
        slot->name = std::move(name);
        slot->created = created ? created : nowSec();
        slot->updated = slot->created;
        if (id.empty()) id = S.NewId();
        S.games[id] = slot;
        return std::pair{id, slot};
    };
    auto cleanName = [](std::string n) {
        const auto b = n.find_first_not_of(" \t\r\n"), e = n.find_last_not_of(" \t\r\n");
        n = b == std::string::npos ? "" : n.substr(b, e - b + 1);
        if (n.size() > 80) throw HttpError{400, "a session name is at most 80 characters"};
        return n;
    };
    // A session as the dropdown shows it. The slot's mutex must be held.
    auto info = [&](const std::string& id, const Slot& s) {
        const Game& g = *s.game;
        const LocationDef* loc = g.Module().FindLocation(g.LocationId());
        const char* role = g.GetRole() == Role::Player ? "player" : g.GetRole() == Role::DungeonMaster ? "dm" : "spectator";
        return Json{{"id", id}, {"name", s.name}, {"module", g.Config().moduleId}, {"moduleName", g.Module().name}, {"hero", g.Hero()}, {"role", role},
                    {"mode", std::string(ToString(g.GetMode()))}, {"location", loc ? loc->name : g.LocationId()}, {"day", g.Day()}, {"created", s.created}, {"updated", s.updated}};
    };
    // Writes the session to <sessions dir>/<id>.json (to a temporary file first, so a crash never leaves half a save). The slot's mutex must be held.
    auto persist = [&](const std::string& id, Slot& s) {
        if (S.sessionsDir.empty()) return;
        namespace fs = std::filesystem;
        try {
            fs::create_directories(S.sessionsDir);
            s.updated = nowSec();
            const Json j{{"format", "cppdnd-session"}, {"version", 1}, {"id", id}, {"name", s.name}, {"created", s.created}, {"updated", s.updated}, {"save", s.game->Save()}};
            const fs::path file = fs::path(S.sessionsDir) / (id + ".json"), tmp = fs::path(S.sessionsDir) / (id + ".json.tmp");
            { std::ofstream f(tmp, std::ios::binary); f << j.dump(); if (!f) throw std::runtime_error("cannot write " + tmp.string()); }
            fs::rename(tmp, file);
        } catch (const std::exception& e) { std::cerr << "warning: cannot save session " << id << ": " << e.what() << "\n"; }
    };

    // Any successful POST to a game's own routes changes its state, so the session file follows.
    svr.set_post_routing_handler([&](const httplib::Request& req, httplib::Response& res) {
        if (req.method != "POST" || res.status >= 300 || S.sessionsDir.empty()) return;
        static const std::regex re(R"(^/api/games/([0-9a-f]{12})(/.*)?$)");
        std::smatch m;
        if (!std::regex_match(req.path, m, re)) return;
        const std::string sub = m[2].str();
        if (sub == "/viewpoint" || sub == "/scene") return;  // pictures and previews do not change the game
        std::shared_ptr<Slot> slot;
        {
            std::lock_guard<std::mutex> lk(S.mapMutex);
            const auto it = S.games.find(m[1].str());
            if (it == S.games.end()) return;
            slot = it->second;
        }
        std::lock_guard<std::mutex> lk(slot->m);
        persist(m[1].str(), *slot);
    });

    svr.Get("/api/health", [](const httplib::Request&, httplib::Response& res) { Reply(res, 200, {{"ok", true}}); });

    svr.Get("/api/content", guarded([&](const httplib::Request&, httplib::Response& res) {
        Json j;
        j["modules"] = Json::array();
        for (const auto& m : Content::Modules())
            j["modules"].push_back({{"id", m.id}, {"name", m.name}, {"setting", m.setting}, {"intro", m.intro}, {"recommendedLevel", m.recommendedLevel}, {"locations", m.locations.size()}});
        j["characters"] = Json::array();
        for (const auto& c : Content::Characters()) j["characters"].push_back({{"id", c.id}, {"name", c.name}, {"class", c.classId}, {"level", c.level}});
        j["monsters"] = Json::array();
        for (const auto& m : Content::Monsters()) j["monsters"].push_back({{"id", m.id}, {"name", m.name}, {"cr", m.cr}, {"xp", m.xp}, {"hp", m.sheet.maxHp}, {"ac", m.sheet.armorClass}});
        j["spells"] = Json::array();
        for (const auto& s : Content::Spells()) j["spells"].push_back({{"id", s.id}, {"name", s.name}, {"level", s.level}, {"concentration", s.concentration}});
        j["classes"] = Json::array();
        for (const auto& c : Content::Classes()) j["classes"].push_back({{"id", c.id}, {"name", c.name}, {"hitDie", c.hitDie}});
        Reply(res, 200, j);
    }));

    svr.Get("/api/llm", guarded([&](const httplib::Request&, httplib::Response& res) {
        const ModelInfo mi = LocateModel(S.llmCfg.model);
        Reply(res, 200, {{"enabled", S.llm != nullptr}, {"agents", Json{{"available", S.agentsCfg.has_value()}, {"file", S.agentsPath}, {"error", S.agentsError}}}, {"client", S.llm ? S.llm->Describe() : ""}, {"url", S.llmCfg.baseUrl}, {"model", S.llmCfg.model},
                         {"modelStore", {{"home", mi.home}, {"path", mi.path}, {"exists", mi.exists}, {"weights", mi.weights}}}});
    }));

    // Models in the shared store (CppLmmModelStore) that an agent can be switched to.
    svr.Get("/api/models", guarded([&](const httplib::Request&, httplib::Response& res) {
        Json arr = Json::array();
        for (const auto& m : ListModels()) arr.push_back({{"name", m.name}, {"path", m.path}, {"weights", m.weights}});
        Reply(res, 200, {{"home", LocateModel("").home}, {"models", arr}});
    }));

    // {games: [ids], sessions: [{id, name, module, moduleName, hero, role, mode, location, day, created, updated}], persistent}, newest first
    svr.Get("/api/games", guarded([&](const httplib::Request&, httplib::Response& res) {
        std::vector<std::pair<std::string, std::shared_ptr<Slot>>> all;
        {
            std::lock_guard<std::mutex> lk(S.mapMutex);
            for (const auto& kv : S.games) all.push_back(kv);
        }
        std::vector<Json> infos;
        Json ids = Json::array();
        for (const auto& [id, slot] : all) {
            std::lock_guard<std::mutex> lk(slot->m);
            ids.push_back(id);
            infos.push_back(info(id, *slot));
        }
        std::sort(infos.begin(), infos.end(), [](const Json& a, const Json& b) { return a["updated"].get<long long>() > b["updated"].get<long long>(); });
        Reply(res, 200, {{"games", ids}, {"sessions", infos}, {"persistent", !S.sessionsDir.empty()}});
    }));

    svr.Post("/api/games", guarded([&](const httplib::Request& req, httplib::Response& res) {
        const Json b = Body(req);
        GameConfig c;
        c.moduleId = b.value("module", "");
        c.rooms = b.value("rooms", 3);
        c.difficulty = b.value("difficulty", "medium");
        c.party = b.value("party", std::vector<std::string>{});
        c.level = b.value("level", 0);
        c.seed = b.value("seed", std::uint64_t{std::random_device{}()});
        c.hero = b.value("hero", "");
        c.pace = b.value("pace", "normal");
        c.llmDm = b.value("llmDm", false);
        c.llmNarration = b.value("llmNarration", false);
        c.agents = b.value("agents", false);
        const auto role = ParseRole(b.value("role", "player"));
        if (!role) throw HttpError{400, "role must be player, dm or spectator"};
        c.role = *role;
        if (c.pace != "slow" && c.pace != "normal" && c.pace != "fast") throw HttpError{400, "pace must be slow, normal or fast"};
        std::unique_ptr<Game> g;
        try { g = std::make_unique<Game>(c); }
        catch (const std::runtime_error& e) { throw HttpError{400, e.what()}; }
        auto table = Wire(S, *g);
        auto [id, slot] = add(std::move(g), cleanName(b.value("name", "")));
        std::lock_guard<std::mutex> lk(slot->m);
        slot->agents = table;
        if (!histDir.empty()) slot->game->SetHistoryDir((std::filesystem::path(histDir) / id).string());
        persist(id, *slot);
        Reply(res, 201, {{"id", id}, {"name", slot->name}, {"view", slot->game->View(0)}});
    }));

    svr.Post("/api/games/load", guarded([&](const httplib::Request& req, httplib::Response& res) {
        const Json b = Body(req);
        std::unique_ptr<Game> g;
        try { g = Game::Load(b.contains("save") ? b.at("save") : b); }
        catch (const Json::exception& e) { throw HttpError{400, std::string("invalid save file: ") + e.what()}; }
        catch (const std::runtime_error& e) { throw HttpError{400, e.what()}; }
        auto table = Wire(S, *g);
        auto [id, slot] = add(std::move(g), b.contains("save") ? cleanName(b.value("name", "")) : "");
        std::lock_guard<std::mutex> lk(slot->m);
        slot->agents = table;
        if (!histDir.empty()) slot->game->SetHistoryDir((std::filesystem::path(histDir) / id).string());
        persist(id, *slot);
        Reply(res, 201, {{"id", id}, {"name", slot->name}, {"view", slot->game->View(0)}});
    }));

    svr.Post("/api/games/:id/name", guarded([&](const httplib::Request& req, httplib::Response& res) {
        auto slot = find(req);
        const std::string n = cleanName(Body(req).value("name", ""));
        if (n.empty()) throw HttpError{400, "a session needs a name"};
        std::lock_guard<std::mutex> lk(slot->m);
        slot->name = n;
        Reply(res, 200, {{"id", req.path_params.at("id")}, {"name", n}});
    }));

    svr.Get("/api/games/:id", guarded([&](const httplib::Request& req, httplib::Response& res) {
        auto slot = find(req);
        std::lock_guard<std::mutex> lk(slot->m);
        Reply(res, 200, slot->game->View(sinceOf(req)));
    }));

    svr.Delete("/api/games/:id", guarded([&](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(S.mapMutex);
        const std::string id = req.path_params.at("id");
        if (!S.games.erase(id)) throw HttpError{404, "no such game"};
        if (!S.sessionsDir.empty() && std::regex_match(id, std::regex("[0-9a-f]{12}"))) {
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(S.sessionsDir) / (id + ".json"), ec);
        }
        Reply(res, 200, {{"deleted", true}});
    }));

    svr.Get("/api/games/:id/save", guarded([&](const httplib::Request& req, httplib::Response& res) {
        auto slot = find(req);
        std::lock_guard<std::mutex> lk(slot->m);
        Reply(res, 200, slot->game->Save());
    }));

    // Runs the fight until a human must answer (or it ends).
    svr.Post("/api/games/:id/advance", guarded([&](const httplib::Request& req, httplib::Response& res) {
        auto slot = find(req);
        std::lock_guard<std::mutex> lk(slot->m);
        if (slot->game->GetMode() != Mode::Combat) throw HttpError{409, "there is no fight to advance"};
        slot->game->AdvanceCombat();
        Reply(res, 200, {{"view", slot->game->View(sinceOf(req))}});
    }));

    svr.Post("/api/games/:id/action", guarded([&](const httplib::Request& req, httplib::Response& res) {
        auto slot = find(req);
        std::lock_guard<std::mutex> lk(slot->m);
        Game& g = *slot->game;
        if (g.GetMode() != Mode::Combat) throw HttpError{409, "there is no fight"};
        Action a;
        std::string err;
        if (!ActionFromJson(Body(req), a, err)) throw HttpError{400, err};
        const ActionResult r = g.SubmitAction(a);
        if (r.ok && g.GetMode() == Mode::Combat) g.AdvanceCombat();
        Reply(res, r.ok ? 200 : 422, {{"ok", r.ok}, {"reason", r.reason}, {"view", g.View(sinceOf(req))}});
    }));

    // Plain English: the model turns the text into commands or actions, the engine validates each one.
    svr.Post("/api/games/:id/say", guarded([&](const httplib::Request& req, httplib::Response& res) {
        auto slot = find(req);
        std::lock_guard<std::mutex> lk(slot->m);
        const std::string text = Body(req).value("text", "");
        const SayResult r = S.llm ? Say(*S.llm, *slot->game, text) : SayOffline(*slot->game, text);  // no model: the rule-based reader
        Reply(res, 200, {{"ok", r.ok}, {"message", r.message}, {"steps", r.steps}, {"view", slot->game->View(sinceOf(req))}});
    }));

    // The agents of this game, their models, and the JSON messages they have exchanged.
    svr.Get("/api/games/:id/agents", guarded([&](const httplib::Request& req, httplib::Response& res) {
        auto slot = find(req);
        std::lock_guard<std::mutex> lk(slot->m);
        if (!slot->agents) { Reply(res, 200, {{"enabled", false}, {"agents", Json::array()}, {"messages", Json::array()}, {"lastSeq", 0}}); return; }
        Reply(res, 200, {{"enabled", true}, {"agents", slot->agents->Describe()}, {"problems", slot->agents->Problems()}, {"messages", slot->agents->Hub().LogJson(sinceOf(req))}, {"lastSeq", slot->agents->Hub().LastSeq()}});
    }));

    // Switch the model of one actor, or of a whole kind of enemy, mid-game.
    // {"selector":"type:undead","model":"phi3:mini","backend":"ollama","fromStore":false}
    svr.Post("/api/games/:id/agents/model", guarded([&](const httplib::Request& req, httplib::Response& res) {
        auto slot = find(req);
        std::lock_guard<std::mutex> lk(slot->m);
        if (!slot->agents) throw HttpError{409, "this game was not created with agents"};
        const Json b = Body(req);
        std::string err;
        const bool ok = slot->agents->SetModel(b.value("selector", ""), b.value("model", ""), b.value("backend", ""), b.value("fromStore", false), err);
        Reply(res, ok ? 200 : 422, {{"ok", ok}, {"error", err}, {"agents", slot->agents->Describe()}, {"problems", slot->agents->Problems()}});
    }));

    svr.Post("/api/games/:id/hero", guarded([&](const httplib::Request& req, httplib::Response& res) {
        auto slot = find(req);
        std::lock_guard<std::mutex> lk(slot->m);
        std::string err;
        if (!slot->game->SetHero(Body(req).value("hero", ""), err)) throw HttpError{422, err};
        Reply(res, 200, {{"view", slot->game->View(sinceOf(req))}});
    }));

    // Histories: GET /history (what exists), /history/group, /history/:x/:y (x's private record of y). ?since=SEQ&limit=N
    auto limitOf = [](const httplib::Request& req) { return req.has_param("limit") ? std::clamp(std::atoi(req.get_param_value("limit").c_str()), 1, 2000) : 200; };
    svr.Get("/api/games/:id/history", guarded([&](const httplib::Request& req, httplib::Response& res) {
        auto slot = find(req);
        std::lock_guard<std::mutex> lk(slot->m);
        Reply(res, 200, slot->game->Hist().Index());
    }));
    svr.Get("/api/games/:id/history/group", guarded([&](const httplib::Request& req, httplib::Response& res) {
        auto slot = find(req);
        std::lock_guard<std::mutex> lk(slot->m);
        Reply(res, 200, slot->game->Hist().GroupJson(sinceOf(req), limitOf(req)));
    }));
    svr.Get("/api/games/:id/history/:x/:y", guarded([&](const httplib::Request& req, httplib::Response& res) {
        auto slot = find(req);
        std::lock_guard<std::mutex> lk(slot->m);
        Reply(res, 200, slot->game->Hist().Pair(req.path_params.at("x"), req.path_params.at("y"), sinceOf(req), limitOf(req)));
    }));

    svr.Post("/api/games/:id/role", guarded([&](const httplib::Request& req, httplib::Response& res) {
        auto slot = find(req);
        std::lock_guard<std::mutex> lk(slot->m);
        const Json b = Body(req);
        const auto role = ParseRole(b.value("role", ""));
        if (!role) throw HttpError{400, "role must be player, dm or spectator"};
        slot->game->SetRole(*role, b.value("hero", ""));
        Reply(res, 200, {{"view", slot->game->View(sinceOf(req))}});
    }));

    // Exploration commands share one shape: run it, report ok/message and the new view.
    auto command = [&](const std::string& path, std::function<CmdResult(Game&, const Json&)> fn) {
        svr.Post("/api/games/:id/" + path, guarded([&, fn](const httplib::Request& req, httplib::Response& res) {
            auto slot = find(req);
            std::lock_guard<std::mutex> lk(slot->m);
            const CmdResult r = fn(*slot->game, Body(req));
            Reply(res, r.ok ? 200 : 422, {{"ok", r.ok}, {"message", r.message}, {"view", slot->game->View(sinceOf(req))}});
        }));
    };
    command("travel", [](Game& g, const Json& b) { return g.Travel(b.value("to", "")); });
    command("search", [](Game& g, const Json&) { return g.Search(); });
    command("talk", [](Game& g, const Json& b) { return g.Talk(b.value("npc", ""), b.value("topic", "")); });
    command("rest", [](Game& g, const Json& b) { return g.Rest(b.value("kind", "short"), b.value("hitDice", 99)); });
    command("wait", [](Game& g, const Json& b) { return g.Wait(b.value("hours", 1)); });
    command("pace", [](Game& g, const Json& b) { return g.SetPace(b.value("pace", "normal")); });
    command("auto", [](Game& g, const Json& b) {
        CmdResult last{true, "ok"};
        for (int i = 0, n = std::clamp(b.value("steps", 1), 1, 200); i < n; ++i) {
            last = g.AutoStep();
            if (!last.ok) break;
        }
        return last;
    });

    if (!artDir.empty()) {  // /art/<kind>/<id>.png, read from disk on each request so new pictures appear without a restart
        std::error_code ec;
        std::filesystem::create_directories(artDir, ec);
        if (!svr.set_mount_point("/art", artDir)) { std::cerr << "warning: cannot serve art from '" << artDir << "'\n"; artDir.clear(); }
    }
    svr.Get("/api/art", [&](const httplib::Request&, httplib::Response& res) {
        namespace fs = std::filesystem;
        Json out{{"base", "/art/"}, {"enabled", !artDir.empty()}};
        for (const char* kind : {"characters", "monsters", "npcs", "locations", "modules"}) {
            Json m = Json::object();
            std::error_code ec;
            const fs::path d = fs::path(artDir) / kind;
            if (!artDir.empty() && fs::is_directory(d, ec))
                for (const auto& e : fs::directory_iterator(d, ec)) {
                    const std::string ext = e.path().extension().string();
                    if (e.is_regular_file(ec) && (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp"))
                        m[e.path().stem().string()] = std::string(kind) + "/" + e.path().filename().string() + "?v=" + std::to_string(e.last_write_time(ec).time_since_epoch().count() % 100000000);
                }
            out[kind] = m;
        }
        Reply(res, 200, out);
    });
    // Every stored picture, newest first (what the Gallery panel shows). "mtime" is an opaque sort key, not a date.
    svr.Get("/api/art/list", [&](const httplib::Request&, httplib::Response& res) {
        namespace fs = std::filesystem;
        struct Row { std::string kind, id, file; std::uintmax_t bytes; long long mtime; };
        std::vector<Row> rows;
        std::error_code ec;
        for (const char* kind : {"characters", "monsters", "npcs", "locations", "modules", "views"}) {
            const fs::path d = fs::path(artDir) / kind;
            if (artDir.empty() || !fs::is_directory(d, ec)) continue;
            for (const auto& e : fs::directory_iterator(d, ec)) {
                const std::string ext = e.path().extension().string();
                if (!e.is_regular_file(ec) || !(ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp")) continue;
                rows.push_back({kind, e.path().stem().string(), e.path().filename().string(), e.file_size(ec), static_cast<long long>(e.last_write_time(ec).time_since_epoch().count())});
            }
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.mtime != b.mtime ? a.mtime > b.mtime : a.kind + a.id < b.kind + b.id; });
        Json items = Json::array();
        for (const auto& r : rows) items.push_back(Json{{"kind", r.kind}, {"id", r.id}, {"url", "/art/" + r.kind + "/" + r.file + "?v=" + std::to_string(r.mtime % 100000000)}, {"bytes", r.bytes}, {"mtime", r.mtime}});
        Reply(res, 200, {{"enabled", !artDir.empty()}, {"count", items.size()}, {"items", items}});
    });
    // ---- image generation: draws into the art folder, one picture at a time, in the background
    std::shared_ptr<ArtService> art;
    std::string imagesError, imagesFile;
    if (!artDir.empty()) {
        namespace fs = std::filesystem;
        imagesFile = !imagesPath.empty() ? imagesPath : (std::getenv("DND_IMAGES") && *std::getenv("DND_IMAGES") ? std::getenv("DND_IMAGES") : (fs::path(dataDir) / "images.json").string());
        auto icfg = ImageConfig::Load(imagesFile, (fs::path(artDir) / "style.json").string(), (fs::path(artDir) / "prompts.json").string(), imagesError);
        if (icfg) art = std::make_shared<ArtService>(*icfg, artDir);
        else std::cerr << "warning: image generation is off: " << imagesError << "\n";
    }
    auto needArt = [&]() -> ArtService& {
        if (!art) throw HttpError{503, imagesError.empty() ? "image generation is off (no art folder)" : imagesError};
        return *art;
    };
    svr.Get("/api/images", guarded([&](const httplib::Request&, httplib::Response& res) {
        if (!art) { Reply(res, 200, {{"enabled", false}, {"error", imagesError}, {"file", imagesFile}}); return; }
        Json s = art->Status();
        s["file"] = imagesFile;
        Reply(res, 200, s);
    }));
    svr.Post("/api/images/generate", guarded([&](const httplib::Request& req, httplib::Response& res) {
        std::string err;
        Json r = needArt().Enqueue(Body(req), err);
        if (!err.empty()) throw HttpError{422, err};
        r["jobs"] = art->Jobs();
        Reply(res, 202, r);
    }));
    svr.Get("/api/images/jobs", guarded([&](const httplib::Request&, httplib::Response& res) { Reply(res, 200, needArt().Jobs()); }));
    svr.Post("/api/images/cancel", guarded([&](const httplib::Request&, httplib::Response& res) {
        needArt().Cancel();
        Reply(res, 200, art->Jobs());
    }));
    svr.Post("/api/images/model", guarded([&](const httplib::Request& req, httplib::Response& res) {
        const Json b = Body(req);
        std::string err;
        if (!needArt().SetCheckpoint(b.value("model", ""), b.value("fromStore", false), err)) throw HttpError{422, err};
        Reply(res, 200, art->Status());
    }));

    // What a camera can see: the battlefield (or an open field when nobody is fighting) with everyone on it, and the hero's eyes
    // as the default camera. The web client draws its live preview from this.
    svr.Get("/api/games/:id/scene", guarded([&](const httplib::Request& req, httplib::Response& res) {
        auto slot = find(req);
        std::lock_guard<std::mutex> lk(slot->m);
        Reply(res, 200, BuildScene(slot->game->View(0)));
    }));
    // Draw what a camera sees. {"camera": {x,y,z,yaw,pitch,fov}} (default: the hero's eyes), optional "init" (a data URL or base64
    // PNG of the live preview, so the picture follows its composition), "strength" 0.05..1, "width", "height", "seed".
    // Returns {id, url-to-poll}; poll GET /api/images/view/:id until its state is done or failed.
    svr.Post("/api/games/:id/viewpoint", guarded([&](const httplib::Request& req, httplib::Response& res) {
        auto slot = find(req);
        const Json b = Body(req);
        Json scene;
        {
            std::lock_guard<std::mutex> lk(slot->m);
            scene = BuildScene(slot->game->View(0));
        }
        const Camera dflt = Camera::FromJson(scene["camera"]);
        const Camera cam = b.contains("camera") && b["camera"].is_object() ? Camera::FromJson(b["camera"], dflt) : dflt;
        ViewJob job;
        auto numOf = [&](const char* k, double d) { return b.contains(k) && b[k].is_number() ? b[k].get<double>() : d; };
        job.width = static_cast<int>(numOf("width", 768));
        job.height = static_cast<int>(numOf("height", 432));
        job.strength = numOf("strength", 0.72);
        job.seed = static_cast<std::uint32_t>(numOf("seed", 0));
        job.subject = DescribeView(scene, cam, job.height > 0 ? static_cast<double>(job.width) / job.height : 16.0 / 9.0);
        if (b.contains("init") && b["init"].is_string()) {
            std::string init = b["init"].get<std::string>();
            if (const auto comma = init.find(','); init.compare(0, 5, "data:") == 0 && comma != std::string::npos) init.erase(0, comma + 1);
            if (init.size() > 12u * 1024 * 1024) throw HttpError{413, "the preview picture is too large"};
            if (!init.empty() && init.compare(0, 11, "iVBORw0KGgo") != 0) throw HttpError{422, "init must be a PNG (base64 or a data URL)"};
            job.initB64 = init;
        }
        job.id = "v" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        std::string err;
        needArt().EnqueueView(job, err);
        if (!err.empty()) throw HttpError{422, err};
        Reply(res, 202, {{"id", job.id}, {"subject", job.subject}, {"camera", cam.ToJson()}, {"state", "queued"}, {"hero", scene["hero"]}});
    }));
    svr.Get("/api/images/view/:id", guarded([&](const httplib::Request& req, httplib::Response& res) {
        Reply(res, 200, needArt().View(req.path_params.at("id")));
    }));

    if (!staticDir.empty() && !svr.set_mount_point("/", staticDir)) {
        std::cerr << "warning: cannot serve web client from '" << staticDir << "'\n";
        staticDir.clear();
    }

    // Restore the sessions a previous run left behind.
    int restored = 0;
    if (!S.sessionsDir.empty() && std::filesystem::is_directory(S.sessionsDir)) {
        namespace fs = std::filesystem;
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(S.sessionsDir))
            if (e.is_regular_file() && e.path().extension() == ".json") files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const auto& p : files) {
            const std::string id = p.stem().string();
            try {
                std::ifstream f(p, std::ios::binary);
                const Json j = Json::parse(f, nullptr, false);
                if (j.is_discarded() || j.value("format", "") != "cppdnd-session" || !std::regex_match(id, std::regex("[0-9a-f]{12}"))) throw std::runtime_error("not a session file");
                std::unique_ptr<Game> g = Game::Load(j.at("save"));
                auto table = Wire(S, *g);
                auto [gid, slot] = add(std::move(g), j.value("name", ""), id, j.value("created", 0LL));
                std::lock_guard<std::mutex> lk(slot->m);
                slot->agents = table;
                slot->updated = j.value("updated", slot->created);
                if (!histDir.empty()) slot->game->SetHistoryDir((fs::path(histDir) / gid).string());
                ++restored;
            } catch (const HttpError& e) { std::cerr << "skipped session " << id << ": " << e.message << "\n"; }
            catch (const std::exception& e) { std::cerr << "skipped session " << id << ": " << e.what() << "\n"; }
        }
    }
    std::cout << "dnd_server on http://" << host << ":" << port << "  data=" << dataDir << "  web=" << (staticDir.empty() ? "off" : staticDir) << "  art=" << (artDir.empty() ? "off" : artDir) << "  history=" << (histDir.empty() ? "off" : histDir) << "  sessions=" << (S.sessionsDir.empty() ? "memory only" : S.sessionsDir) << " (" << restored << " restored)" << "  images=" << (art ? "on" : "off") << "  modules=" << Content::Modules().size()
              << "  llm=" << (S.llm ? S.llm->Describe() : "off") << std::endl;
    if (!svr.listen(host, port)) {
        std::cerr << "cannot listen on " << host << ":" << port << "\n";
        return 1;
    }
    return 0;
}
