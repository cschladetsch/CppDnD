// dnd_server: HTTP/JSON backend for a web front end. See docs/API.md.
#include <atomic>
#include <csignal>
#include <iostream>
#include <map>
#include <mutex>
#include <random>

#include <filesystem>

#include "dnd/Agents.hpp"
#include "dnd/Content.hpp"
#include "dnd/Game.hpp"
#include "dnd/Llm.hpp"
#include "httplib.h"

using namespace dnd;
using Json = nlohmann::json;

namespace {

struct Slot {
    std::mutex m;
    std::unique_ptr<Game> game;
    std::shared_ptr<AgentTable> agents;  // set when the game was created with "agents": true
};

struct Server {
    std::mutex mapMutex;
    std::map<std::string, std::shared_ptr<Slot>> games;
    std::shared_ptr<ILlmClient> llm;  // null: LLM features disabled
    LlmConfig llmCfg;
    bool mock = false;
    std::optional<AgentsConfig> agentsCfg;  // from --agents / DND_AGENTS / <data>/agents.json
    std::string agentsPath, agentsError;
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
    std::string staticDir, host = "127.0.0.1";
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
        else if (a == "--llm-url") S.llmCfg.baseUrl = next();
        else if (a == "--llm-model") S.llmCfg.model = next();
        else if (a == "--mock-llm") S.mock = true;
        else if (a == "--agents") S.agentsPath = next();
        else if (a == "--ollama") { S.llmCfg.baseUrl = "http://127.0.0.1:11434"; S.llmCfg.model = next(); S.llmCfg.stream = false; }
        else if (a == "--help" || a == "-h") {
            std::cout << "dnd_server [--port 8088] [--host 127.0.0.1] [--data DIR] [--static DIR]\n"
                         "           [--llm-url http://127.0.0.1:8080] [--llm-model NAME] [--ollama MODEL] [--agents FILE] [--mock-llm]\n"
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
    auto add = [&](std::unique_ptr<Game> g) {
        std::lock_guard<std::mutex> lk(S.mapMutex);
        if (S.games.size() >= 256) throw HttpError{429, "too many games; delete some first"};
        auto slot = std::make_shared<Slot>();
        slot->game = std::move(g);
        std::string id = S.NewId();
        S.games[id] = slot;
        return std::pair{id, slot};
    };

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

    svr.Get("/api/games", guarded([&](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(S.mapMutex);
        Json a = Json::array();
        for (const auto& [id, slot] : S.games) a.push_back(id);
        Reply(res, 200, {{"games", a}});
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
        auto [id, slot] = add(std::move(g));
        std::lock_guard<std::mutex> lk(slot->m);
        slot->agents = table;
        Reply(res, 201, {{"id", id}, {"view", slot->game->View(0)}});
    }));

    svr.Post("/api/games/load", guarded([&](const httplib::Request& req, httplib::Response& res) {
        const Json b = Body(req);
        std::unique_ptr<Game> g;
        try { g = Game::Load(b.contains("save") ? b.at("save") : b); }
        catch (const Json::exception& e) { throw HttpError{400, std::string("invalid save file: ") + e.what()}; }
        catch (const std::runtime_error& e) { throw HttpError{400, e.what()}; }
        auto table = Wire(S, *g);
        auto [id, slot] = add(std::move(g));
        std::lock_guard<std::mutex> lk(slot->m);
        slot->agents = table;
        Reply(res, 201, {{"id", id}, {"view", slot->game->View(0)}});
    }));

    svr.Get("/api/games/:id", guarded([&](const httplib::Request& req, httplib::Response& res) {
        auto slot = find(req);
        std::lock_guard<std::mutex> lk(slot->m);
        Reply(res, 200, slot->game->View(sinceOf(req)));
    }));

    svr.Delete("/api/games/:id", guarded([&](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(S.mapMutex);
        if (!S.games.erase(req.path_params.at("id"))) throw HttpError{404, "no such game"};
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
        if (!S.llm) throw HttpError{400, "no model configured (start the server with --ollama MODEL, or set DND_LLM_URL / DND_LLM_MODEL)"};
        auto slot = find(req);
        std::lock_guard<std::mutex> lk(slot->m);
        const SayResult r = Say(*S.llm, *slot->game, Body(req).value("text", ""));
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

    if (!staticDir.empty() && !svr.set_mount_point("/", staticDir)) {
        std::cerr << "warning: cannot serve web client from '" << staticDir << "'\n";
        staticDir.clear();
    }

    std::cout << "dnd_server on http://" << host << ":" << port << "  data=" << dataDir << "  web=" << (staticDir.empty() ? "off" : staticDir) << "  modules=" << Content::Modules().size()
              << "  llm=" << (S.llm ? S.llm->Describe() : "off") << std::endl;
    if (!svr.listen(host, port)) {
        std::cerr << "cannot listen on " << host << ":" << port << "\n";
        return 1;
    }
    return 0;
}
