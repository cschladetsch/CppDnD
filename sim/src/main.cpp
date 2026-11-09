// dnd_sim: terminal front end. The same Game drives the HTTP server (dnd_server).
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>

#include "dnd/Content.hpp"
#include "dnd/Game.hpp"
#include "dnd/Agents.hpp"
#include "dnd/Human.hpp"
#include "dnd/Llm.hpp"

using namespace dnd;

namespace {

void Usage() {
    std::cout <<
        "dnd_sim - D&D 5e simulator (terminal)\n"
        "  --module <id>             play a module (see --list); default: a procedural delve\n"
        "  --rooms <n> --difficulty easy|medium|hard|deadly   procedural delve settings\n"
        "  --role player|dm|auto     who you are (default: auto = watch the AI play)\n"
        "  --interactive, -i         play it yourself (same as --role player unless --role is given)\n"
        "  --hero <id|name>          hero to control in player mode\n"
        "  --party a,b,c             character ids (default: thorin,elara,aldric,finn)\n"
        "  --level <n>               override starting level\n"
        "  --pace slow|normal|fast   travel pace\n"
        "  --seed <n>                reproducible run\n"
        "  --data <dir>              content directory (default: $DND_DATA or the built-in path)\n"
        "  --load <file> / --save <file>   resume / write a save file\n"
        "  --llm-narration / --llm-dm      use the model from DND_LLM_URL / DND_LLM_MODEL\n"
        "  --llm-say                 type plain English at the prompt (same model settings)\n"
        "  --history DIR             write each pair's private x-y.json and group-history.json to DIR\n"
        "  --agents [file]           every actor is its own model (default file: <data>/agents.json or DND_AGENTS)\n"
        "  --quiet                   no narration\n"
        "  --list                    list modules, characters and monsters\n"
        "In exploration type 'help'; in a fight type 'help' at your turn. 'swap player|dm|auto' changes role mid-fight.\n";
}

void Print(const Event& e) {
    switch (e.type) {
        case EventType::CombatStart: std::cout << "\n*** " << e.text << " ***\n"; break;
        case EventType::RoundStart: std::cout << "\n=== Round " << e.amount << " ===\n"; break;
        case EventType::TurnStart: std::cout << "-- " << e.text << "\n"; break;
        case EventType::CombatEnd: std::cout << "\n*** " << e.text << " ***\n"; break;
        case EventType::Scene: std::cout << "\n" << e.text << "\n"; break;
        case EventType::Narration: std::cout << "\n  " << e.text << "\n"; break;
        case EventType::Travel: case EventType::Rest: case EventType::Quest: std::cout << "\n* " << e.text << "\n"; break;
        default: std::cout << "   " << e.text << "\n"; break;
    }
    if (!e.flavour.empty()) std::cout << "   (" << e.flavour << ")\n";
}

void ShowLocation(const Game& g) {
    const auto v = g.View(g.LastSeq());
    const auto& l = v["location"];
    std::cout << "\n[" << l["name"].get<std::string>() << "]  day " << v["clock"]["day"] << ", " << v["clock"]["clockHour"] << ":00"
              << (v["clock"]["night"].get<bool>() ? " (night)" : "") << "  rest: " << l["rest"].get<std::string>() << "\n";
    for (const auto& e : l["exits"]) {
        std::cout << "  go " << e["to"].get<std::string>() << "  - " << e["name"].get<std::string>();
        if (!e["label"].get<std::string>().empty()) std::cout << " (" << e["label"].get<std::string>() << ")";
        if (!e["available"].get<bool>()) std::cout << "  [blocked]";
        std::cout << "\n";
    }
    for (const auto& n : l["npcs"]) {
        std::cout << "  talk " << n["id"].get<std::string>() << " <topic>  - " << n["name"].get<std::string>() << ": ";
        for (const auto& t : n["topics"]) std::cout << t["id"].get<std::string>() << ' ';
        std::cout << "\n";
    }
}

void ShowParty(const Game& g) {
    for (const auto& pm : g.Party()) {
        const auto& s = pm.sheet;
        std::cout << "  " << s.name << " (" << s.classId << " " << s.level << ")  HP " << s.hp << "/" << s.maxHp << "  XP " << pm.xp
                  << (s.exhaustion ? "  exhaustion " + std::to_string(s.exhaustion) : "") << (pm.dead ? "  DEAD" : "") << "\n";
    }
}

void ShowQuests(const Game& g) {
    for (const auto& q : g.Module().quests) {
        const auto v = g.View(g.LastSeq());
        for (const auto& x : v["quests"])
            if (x["id"] == q.id) std::cout << "  [" << (x["done"].get<bool>() ? "x" : " ") << "] " << q.name << " - " << q.description << "\n";
    }
}

void RunCombat(Game& g) {
    while (g.GetMode() == Mode::Combat) {
        if (g.AdvanceCombat() == StepStatus::AwaitingInput) {
            std::cerr << "(a remote controller is awaiting input; the CLI hands the fight to the AI)\n";
            g.SetRole(Role::Spectator);
        }
    }
}

void Explore(Game& g, const std::string& savePath, ILlmClient* say) {
    std::cout << "Commands: look, go <place>, search, talk <npc> [topic], rest short|long, wait <hours>, pace <slow|normal|fast>, party, hero [name], quests, auto [n], save <file>, quit\n";
    ShowLocation(g);
    std::string line;
    while (g.GetMode() == Mode::Exploring || g.GetMode() == Mode::Combat) {
        if (g.GetMode() == Mode::Combat) { RunCombat(g); if (g.GetMode() == Mode::Exploring) ShowLocation(g); continue; }
        g.FlushHistory();
        std::cout << "\n> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        std::istringstream ss(line);
        std::string cmd, a, b;
        ss >> cmd >> a >> b;
        CmdResult r{true, ""};
        if (cmd.empty()) continue;
        if (cmd == "quit" || cmd == "exit") break;
        else if (cmd == "help") std::cout << "see the list above; 'look' shows exits and people\n";
        else if (cmd == "look") ShowLocation(g);
        else if (cmd == "party") ShowParty(g);
        else if (cmd == "hero") {
            std::string err;
            if (a.empty()) std::cout << "  you control " << g.Hero() << "; 'hero NAME' to switch\n";
            else if (!g.SetHero(a, err)) std::cout << "  (" << err << ")\n";
            else std::cout << "  you now control " << g.Hero() << "\n";
        }
        else if (cmd == "quests") ShowQuests(g);
        else if (cmd == "go") r = g.Travel(a);
        else if (cmd == "search") r = g.Search();
        else if (cmd == "talk") r = g.Talk(a, b);
        else if (cmd == "rest") r = g.Rest(a.empty() ? "short" : a);
        else if (cmd == "wait") r = g.Wait(a.empty() ? 1 : std::atoi(a.c_str()));
        else if (cmd == "pace") r = g.SetPace(a);
        else if (cmd == "auto") { for (int i = 0, n = a.empty() ? 1 : std::atoi(a.c_str()); i < n && g.GetMode() == Mode::Exploring; ++i) { r = g.AutoStep(); if (!r.ok) break; } }
        else if (cmd == "save") { std::ofstream(a.empty() ? savePath : a) << g.Save().dump(1); std::cout << "saved\n"; }
        else if (say) {
            const SayResult sr = Say(*say, g, line);
            if (!sr.message.empty()) std::cout << "  " << sr.message << "\n";
            if (g.GetMode() == Mode::Exploring) ShowLocation(g);
        }
        else std::cout << "unknown command '" << cmd << "'\n";
        if (!r.ok && !r.message.empty()) std::cout << "  (" << r.message << ")\n";
        if (cmd == "talk" && r.ok && b.empty()) std::cout << "  " << r.message << "\n";
        if (g.GetMode() == Mode::Exploring && (cmd == "go" || cmd == "auto")) ShowLocation(g);
    }
}

}  // namespace

int main(int argc, char** argv) {
    GameConfig cfg;
    cfg.role = Role::Spectator;
    cfg.seed = std::random_device{}();
    std::string dataDir = Content::DefaultDataDir(), loadPath, savePath;
    bool quiet = false, list = false, llmSay = false, interactive = false, useAgents = false;
    std::string agentsPath, historyDir;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--module") cfg.moduleId = next();
        else if (a == "--rooms") cfg.rooms = std::atoi(next().c_str());
        else if (a == "--difficulty") cfg.difficulty = next();
        else if (a == "--role") {
            const std::string r = next();
            cfg.role = (r == "auto") ? Role::Spectator : ParseRole(r).value_or(Role::Spectator);
        }
        else if (a == "--hero") cfg.hero = next();
        else if (a == "--party") { std::stringstream ss(next()); for (std::string p; std::getline(ss, p, ',');) cfg.party.push_back(p); }
        else if (a == "--level") cfg.level = std::atoi(next().c_str());
        else if (a == "--pace") cfg.pace = next();
        else if (a == "--seed") cfg.seed = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--data") dataDir = next();
        else if (a == "--load") loadPath = next();
        else if (a == "--save") savePath = next();
        else if (a == "--llm-narration") cfg.llmNarration = true;
        else if (a == "--llm-dm") cfg.llmDm = true;
        else if (a == "--llm-say") llmSay = true;
        else if (a == "--agents") { useAgents = true; if (i + 1 < argc && argv[i + 1][0] != '-') agentsPath = argv[++i]; }
        else if (a == "--interactive" || a == "-i") interactive = true;
        else if (a == "--history") historyDir = (i + 1 < argc) ? argv[++i] : "";
        else if (a == "--quiet") quiet = true;
        else if (a == "--list") list = true;
        else if (a == "--help" || a == "-h") { Usage(); return 0; }
        else { std::cerr << "unknown option " << a << "\n"; Usage(); return 2; }
    }

    try {
        Content::Load(dataDir);
        if (list) {
            std::cout << "modules:\n";
            for (const auto& m : Content::Modules()) std::cout << "  " << m.id << "  " << m.name << " (level " << m.recommendedLevel << ", " << m.locations.size() << " locations)\n";
            std::cout << "characters:\n";
            for (const auto& c : Content::Characters()) std::cout << "  " << c.id << "  " << c.name << " (" << c.classId << ")\n";
            std::cout << "monsters:\n";
            for (const auto& m : Content::Monsters()) std::cout << "  " << m.id << "  CR " << m.cr << "\n";
            return 0;
        }
        DiceRoller::Seed(cfg.seed);
        cfg.agents = useAgents;

        std::unique_ptr<Game> g;
        if (!loadPath.empty()) {
            std::ifstream in(loadPath);
            if (!in) { std::cerr << "cannot open " << loadPath << "\n"; return 1; }
            g = Game::Load(Json::parse(in));
        } else {
            g = std::make_unique<Game>(cfg);   // prints nothing yet: the listener is attached below
        }

        if (interactive && g->Config().role == Role::Spectator) g->SetRole(Role::Player, cfg.hero);
        if (!historyDir.empty()) g->SetHistoryDir(historyDir);

        const auto human = [] { return std::make_unique<HumanController>(std::cin, std::cout); };
        g->SetHumanFactory(human);
        if (!quiet) g->SetListener(Print);

        std::shared_ptr<ILlmClient> llm;
        if (g->Config().llmNarration || g->Config().llmDm || llmSay) {
            const LlmConfig lc = LlmConfig::FromEnv();
            llm = std::make_shared<HttpLlmClient>(lc);
            if (g->Config().llmNarration) g->SetNarrator(MakeLlmNarrator(llm, *g));
            if (g->Config().llmDm) g->SetMonsterFactory([llm] { return std::make_unique<LlmDmController>(llm); });
            std::cerr << "LLM: " << llm->Describe() << "  model store: " << LocateModel(lc.model).path << "\n";
        }

        if (g->Config().agents) {
            if (agentsPath.empty()) agentsPath = std::getenv("DND_AGENTS") && *std::getenv("DND_AGENTS") ? std::getenv("DND_AGENTS") : dataDir + "/agents.json";
            std::string err;
            auto acfg = AgentsConfig::Load(agentsPath, err);
            if (!acfg) { std::cerr << err << "\n"; return 2; }
            auto table = std::make_shared<AgentTable>(*acfg);
            AgentTable::Attach(table, *g);
            std::cerr << "AI table (" << agentsPath << "):\n";
            for (const auto& a : table->Describe()) std::cerr << "  " << a["name"].get<std::string>() << " [" << a["selector"].get<std::string>() << "] " << a["model"].get<std::string>() << "\n";
            for (const auto& p : table->Problems()) std::cerr << "  problem: " << p.get<std::string>() << "\n";
        }

        // Replay what happened during construction (the intro), now that a listener exists.
        if (!quiet && loadPath.empty())
            for (const auto& e : g->Log()) Print(e);

        if (g->Config().role == Role::Spectator) {
            for (int i = 0; i < 5000 && (g->GetMode() == Mode::Exploring || g->GetMode() == Mode::Combat); ++i)
                if (!g->AutoStep().ok) break;
        } else {
            Explore(*g, savePath.empty() ? "save.json" : savePath, llmSay ? llm.get() : nullptr);
        }
        std::cout << "\nResult: " << ToString(g->GetMode()) << " after " << g->Day() << " day(s).\n";
        ShowParty(*g);
        if (!savePath.empty()) { std::ofstream(savePath) << g->Save().dump(1); std::cout << "saved to " << savePath << "\n"; }
        return g->GetMode() == Mode::Defeated ? 3 : 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
