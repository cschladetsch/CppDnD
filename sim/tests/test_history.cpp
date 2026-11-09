#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#include "dnd/AI.hpp"
#include "dnd/Agents.hpp"
#include "dnd/Combat.hpp"
#include "dnd/Content.hpp"
#include "dnd/DM.hpp"
#include "dnd/Game.hpp"
#include "dnd/Human.hpp"
#include "dnd/Images.hpp"
#include "dnd/Json.hpp"
#include "dnd/Llm.hpp"
#include "dnd/Scene.hpp"
#include "test_support.hpp"

using namespace dnd;


TEST(History, PairsAndGroup) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "dnd_history_test";
    fs::remove_all(dir);
    auto readJson = [&](const std::string& name) {
        std::ifstream in(dir / name);
        return Json::parse(in, nullptr, false);
    };

    // ---- the class on its own: x-y is x's view, y-x is y's view, group is separate
    {
        History h;
        h.Between("Thorin", "Elara", "tell", "Stay behind me.", {1, 18});
        h.Group("Thorin", "say", "Ready?", {1, 18});
        Json xy = h.Pair("Thorin", "Elara"), yx = h.Pair("elara", "THORIN");  // names match without case
        EXPECT_TRUE(xy["owner"] == "Thorin" && xy["entries"].size() == 1 && xy["entries"][0]["dir"] == "to");
        EXPECT_TRUE(yx["owner"] == "Elara" && yx["entries"].size() == 1 && yx["entries"][0]["dir"] == "from" && yx["entries"][0]["speaker"] == "Thorin");
        EXPECT_TRUE(h.Pair("Thorin", "Finn")["entries"].empty());
        EXPECT_TRUE(h.GroupJson()["entries"].size() == 1 && h.GroupJson()["entries"][0]["text"] == "Ready?");
        h.Between("Thorin", "Thorin", "say", "to myself", {1, 18});  // nobody talks to themselves in a pair file
        EXPECT_EQ(h.Index()["pairs"].size(), 2);
        History r = History::Restore(h.Dump());
        EXPECT_EQ(r.Dump(), h.Dump());
        EXPECT_TRUE(h.Pair("Thorin", "Elara", h.Pair("Thorin", "Elara")["entries"][0]["seq"].get<int>())["entries"].empty());  // since
        Json rec = h.Recent("Elara");
        EXPECT_TRUE(rec["group"].size() == 1 && rec["with"].contains("Thorin") && rec["with"]["Thorin"][0]["text"] == "Stay behind me.");
        EXPECT_EQ(History::PairFile("Marta Hollin", "Thorin"), "marta_hollin-thorin.json");
    }

    // ---- a conversation with an NPC lands in both private files and writes real files
    {
        GameConfig mc;
        mc.moduleId = "ravenmoor"; mc.seed = 5; mc.role = Role::Player;
        Game g(mc);
        g.SetHistoryDir(dir.string());
        EXPECT_TRUE(g.Travel("greywick").ok && g.Talk("marta", "rumours").ok);
        g.FlushHistory();
        Json t = readJson("thorin-marta_hollin.json"), m = readJson("marta_hollin-thorin.json"), grp = readJson("group-history.json");
        EXPECT_TRUE(t.is_object() && t["owner"] == "Thorin" && t["with"] == "Marta Hollin" && t["entries"].size() == 2);
        EXPECT_TRUE(t["entries"][0]["kind"] == "ask" && t["entries"][0]["dir"] == "to" && t["entries"][1]["kind"] == "reply" && t["entries"][1]["dir"] == "from");
        EXPECT_TRUE(m.is_object() && m["owner"] == "Marta Hollin" && m["entries"][0]["dir"] == "from" && m["entries"][1]["dir"] == "to");
        EXPECT_TRUE(grp.is_object() && grp["entries"].size() >= 2 && grp["entries"][0]["kind"] == "scene");  // intro and the walk to the village
        // switching who you control changes whose side the next conversation is written from
        std::string err;
        EXPECT_TRUE(g.SetHero("elara", err) && g.Talk("marta", "rumours").ok);
        g.FlushHistory();
        EXPECT_EQ(readJson("elara-marta_hollin.json")["entries"].size(), 2);
        // saves carry the history
        EXPECT_EQ(Game::Load(g.Save())->Hist().Dump(), g.Hist().Dump());
    }
    fs::remove_all(dir);

    // ---- fights are recorded between the two who fought, from each side
    {
        GameConfig fc;
        fc.rooms = 1; fc.difficulty = "easy"; fc.seed = 3; fc.role = Role::Spectator;
        Game g(fc);
        int guard = 0;
        while (g.GetMode() == Mode::Combat && guard++ < 500) g.AdvanceCombat();
        Json idx = g.Hist().Index();
        bool sawAttack = false;
        for (const auto& p : idx["pairs"]) {
            Json mine = g.Hist().Pair(p["owner"], p["with"]), theirs = g.Hist().Pair(p["with"], p["owner"]);
            EXPECT_EQ(mine["entries"].size(), theirs["entries"].size());
            for (const auto& e : mine["entries"]) sawAttack = sawAttack || e["kind"] == "attack" || e["kind"] == "spell";
        }
        EXPECT_TRUE(sawAttack);
        bool announced = false;
        const Json grp = g.Hist().GroupJson();
        for (const auto& e : grp["entries"]) announced = announced || e["kind"] == "combat";
        EXPECT_TRUE(announced);
    }

    // ---- agents: private "tell" goes to one companion only, "say" to the table, and agents are shown their history
    {
        std::string err;
        auto cfg = AgentsConfig::Load(std::string(DND_DATA_DIR) + "/agents.json", err);
        EXPECT_TRUE(cfg.has_value());
        if (!cfg) return;
        std::map<std::string, std::string> lastUser;
        int finnCalls = 0;
        AgentTable::ClientFactory factory = [&](const std::string& name, const BackendDef&, const AgentDef&) -> std::shared_ptr<ILlmClient> {
            return std::make_shared<MockLlmClient>([&, name](const std::string&, const std::string& user, bool) {
                LlmReply r;
                r.ok = true;
                lastUser[name] = user;
                if (name == "Finn" && ++finnCalls == 1) r.content = R"({"say":"Search first?","tell":[{"to":"Elara","text":"I do not trust the innkeeper."},{"to":"Nobody","text":"lost"}],"propose":{"cmd":"search"}})";
                else r.content = R"({"say":"To the village.","propose":{"cmd":"travel","to":"greywick"}})";
                return r;
            });
        };
        GameConfig mc;
        mc.moduleId = "ravenmoor"; mc.seed = 5; mc.role = Role::Spectator; mc.agents = true;
        Game g(mc);
        auto table = std::make_shared<AgentTable>(*cfg, factory);
        AgentTable::Attach(table, g);
        EXPECT_TRUE(g.AutoStep().ok);
        Json fe = g.Hist().Pair("Finn", "Elara"), ef = g.Hist().Pair("Elara", "Finn");
        EXPECT_TRUE(fe["entries"].size() == 1 && fe["entries"][0]["kind"] == "tell" && fe["entries"][0]["dir"] == "to");
        EXPECT_TRUE(ef["entries"].size() == 1 && ef["entries"][0]["dir"] == "from" && ef["entries"][0]["text"] == "I do not trust the innkeeper.");
        EXPECT_TRUE(g.Hist().Pair("Finn", "Thorin")["entries"].empty() && g.Hist().Pair("Thorin", "Elara")["entries"].empty());  // private
        EXPECT_EQ(g.Hist().Index()["pairs"].size(), 2);  // the made-up recipient was dropped
        bool said = false, proposed = false;
        const Json grp = g.Hist().GroupJson();
        for (const auto& e : grp["entries"]) {
            said = said || (e["speaker"] == "Finn" && e["text"] == "Search first?");
            proposed = proposed || e["kind"] == "proposal";
        }
        EXPECT_TRUE(said && proposed);
        for (const auto& e : g.Log()) EXPECT_EQ(e.text.find("I do not trust"), std::string::npos);  // private lines are not in the public log
        // the next time the party deliberates, Elara is shown what Finn told her and what was said at the table
        g.Travel("greywick");
        EXPECT_TRUE(g.AutoStep().ok);
        const Json seen = Json::parse(lastUser["Elara"], nullptr, false);
        EXPECT_TRUE(seen.is_object() && seen.contains("history") && seen["history"]["with"].contains("Finn"));
        EXPECT_TRUE(seen["history"]["group"].is_array());
    }
}
