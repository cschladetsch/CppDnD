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


TEST(Llm, Pipeline) {
    EXPECT_EQ(StripThink("<think>plotting</think>  Hello"), "Hello");
    EXPECT_EQ(StripThink("reasoning leaked</think>Answer"), "Answer");
    EXPECT_EQ(ExtractJsonObject("sure! ```json\n{\"a\":{\"b\":\"}\"}}\n``` done"), "{\"a\":{\"b\":\"}\"}}");
    std::string err;
    auto plan = LlmDmController::ParsePlan("```json\n{\"actions\":[{\"type\":\"dash\"},{\"type\":\"end_turn\"}]}\n```", err);
    EXPECT_TRUE(plan.size() == 2 && plan[0].type == ActionType::Dash);
    EXPECT_TRUE(LlmDmController::ParsePlan("I attack!", err).empty() && !err.empty());
    EXPECT_TRUE(LlmDmController::ParsePlan(R"({"actions":[{"type":"explode"}]})", err).empty());

    auto runWith = [](std::shared_ptr<MockLlmClient> mock, bool dm, bool narration) {
        GameConfig cfg;
        cfg.rooms = 1; cfg.difficulty = "easy"; cfg.seed = 3; cfg.role = Role::Spectator;
        cfg.llmDm = dm; cfg.llmNarration = narration;
        Game g(cfg);
        if (narration) g.SetNarrator(MakeLlmNarrator(mock, g));
        if (dm) g.SetMonsterFactory([mock] { return std::make_unique<LlmDmController>(mock); });
        int guard = 0;
        while (g.GetMode() == Mode::Combat && guard++ < 500) g.AdvanceCombat();
        EXPECT_NE(g.GetMode(), Mode::Combat);  // the model can never stall a fight
        return g.Save();
    };
    // Garbage replies fall back to the built-in AI, so the result equals a plain AI run.
    auto junk = std::make_shared<MockLlmClient>([](const std::string&, const std::string&, bool) { LlmReply r; r.ok = true; r.content = "no idea, sorry"; return r; });
    auto failing = std::make_shared<MockLlmClient>([](const std::string&, const std::string&, bool) { LlmReply r; r.error = "boom"; return r; });
    const auto plain = runWith(std::make_shared<MockLlmClient>(), false, false);
    const auto viaJunk = runWith(junk, true, false);
    EXPECT_TRUE(viaJunk["rng"] == plain["rng"] && viaJunk["party"] == plain["party"]);
    EXPECT_TRUE(junk->Calls() > 0);
    runWith(failing, true, true);
    // A well-formed plan is executed through the same rules engine.
    auto smart = std::make_shared<MockLlmClient>([](const std::string&, const std::string& user, bool json) {
        LlmReply r; r.ok = true;
        r.content = json ? R"({"actions":[{"type":"dodge"},{"type":"end_turn"}]})" : "Torchlight shivers.";
        return r;
    });
    const auto out = runWith(smart, true, true);
    bool flavoured = false;
    for (const auto& e : out["log"]) flavoured = flavoured || !e["flavour"].get<std::string>().empty();
    EXPECT_TRUE(flavoured);
    // The model store resolves paths even when a model is absent.
    const auto mi = LocateModel("no-such-model");
    EXPECT_TRUE(!mi.exists && mi.path.find("no-such-model") != std::string::npos);
}

TEST(Llm, Say) {
    GameConfig cfg;
    cfg.moduleId = "ravenmoor";
    cfg.seed = 11;
    Game g(cfg);
    MockLlmClient mock([](const std::string&, const std::string& user, bool) {
        LlmReply r;
        r.ok = true;
        r.content = user.find("Player says: leave") != std::string::npos
                        ? R"(```json
{"steps":[{"cmd":"travel","to":"greywick"},{"cmd":"travel","to":"nowhere-real"}],"reply":"Off we go."})"
                        : R"({"steps":[],"reply":"A cold wind answers."})";
        return r;
    });
    const std::string before = g.LocationId();
    SayResult chat = Say(mock, g, "what do you think?");
    EXPECT_TRUE(chat.ok && chat.steps.empty() && chat.message == "A cold wind answers." && g.LocationId() == before);
    SayResult go = Say(mock, g, "leave for the village");
    EXPECT_EQ(g.LocationId(), "greywick");      // the first step ran
    EXPECT_TRUE(!go.ok && go.steps.size() == 1);    // the invented exit was refused by the engine
    EXPECT_EQ(Say(mock, g, "   ").ok, false);
    {   // an attack asked for while exploring is refused as "no fight", not as somebody else's turn
        GameConfig ec; ec.moduleId = "ravenmoor"; ec.seed = 11;
        Game eg(ec);
        MockLlmClient attack([](const std::string&, const std::string&, bool) { LlmReply r; r.ok = true; r.content = R"({"steps":[{"type":"attack","target":1,"attack":0}],"reply":"You swing."})"; return r; });
        const SayResult sr = Say(attack, eg, "attack the wolves");
        EXPECT_TRUE(!sr.ok && sr.message.find("No fight is under way") != std::string::npos && sr.message.find("turn") != std::string::npos);
        EXPECT_EQ(sr.message.find("not your turn"), std::string::npos);
        // in a fight but between turns it names whose turn it is
        GameConfig cc; cc.rooms = 1; cc.difficulty = "easy"; cc.seed = 11; cc.role = Role::Player; cc.hero = "elara";
        Game cg(cc);
        const SayResult cs = Say(attack, cg, "attack");
        EXPECT_TRUE(!cs.ok && (cs.message.find("'s turn") != std::string::npos || cs.message.find("between turns") != std::string::npos || cs.message.find("not possible") != std::string::npos));
    }
    MockLlmClient broken([](const std::string&, const std::string&, bool) { LlmReply r; r.ok = false; r.error = "down"; return r; });
    EXPECT_FALSE(Say(broken, g, "hello").ok);
}

TEST(Llm, Agents) {
    // ---- the shipped agents.json parses and resolves as documented
    std::string err;
    auto cfg = AgentsConfig::Load(std::string(DND_DATA_DIR) + "/agents.json", err);
    EXPECT_TRUE(cfg.has_value());
    if (!cfg) { std::cerr << err << "\n"; return; }
    EXPECT_EQ(cfg->ForPlayer("thorin", "Thorin").model, "dolphin-llama3:8b");
    EXPECT_EQ(cfg->ForPlayer("nobody", "Nobody").model, "qwen2.5:7b");
    auto undead = cfg->ForMonster("zombie", {"undead"});
    EXPECT_TRUE(undead.first == "type:undead" && undead.second.model == "dolphin-llama3:8b");
    auto vamp = cfg->ForMonster("vampire_spawn", {"undead"});
    EXPECT_TRUE(vamp.first == "monster:vampire_spawn" && vamp.second.model == "qwen2.5:7b");
    EXPECT_EQ(cfg->ForMonster("goblin", {"humanoid"}).second.model, "phi3:mini");
    EXPECT_EQ(cfg->ForMonster("mystery", {"aberration"}).first, "dm");
    EXPECT_EQ(cfg->ForNpc("marta", "Marta Hollin", "innkeeper").model, "phi3:mini");
    EXPECT_EQ(cfg->ForNpc("x", "X", "villain").model, "qwen2.5:7b");
    EXPECT_EQ(cfg->ForNpc("x", "X", "mystery").model, "gemma2:2b");
    EXPECT_TRUE(!AgentsConfig::Parse(Json::parse(R"({"defaults":{"model":"m","backend":"nope"}})"), err) && err.find("backend") != std::string::npos);
    EXPECT_TRUE(!AgentsConfig::Parse(Json::parse(R"({"backends":{"o":{}},"defaults":{"backend":"o"}})"), err) && err.find("model") != std::string::npos);

    // ---- every actor is its own mind: own client, own persona, own model
    struct Mind { std::string model, system; int made = 0, calls = 0; };
    std::map<std::string, Mind> minds;
    std::function<std::string(const std::string&, const std::string&)> replyFn;
    AgentTable::ClientFactory factory = [&](const std::string& name, const BackendDef&, const AgentDef& def) {
        minds[name].model = def.model;
        ++minds[name].made;
        return std::make_shared<MockLlmClient>([&minds, &replyFn, name](const std::string& sys, const std::string& user, bool) {
            minds[name].system = sys;
            ++minds[name].calls;
            LlmReply r;
            r.ok = true;
            r.content = replyFn(name, user);
            return r;
        });
    };
    auto makeGame = [&](bool agents, GameConfig c) {
        c.agents = agents;
        return std::make_unique<Game>(c);
    };
    GameConfig dc;
    dc.rooms = 1; dc.difficulty = "easy"; dc.seed = 3; dc.role = Role::Spectator;

    replyFn = [](const std::string&, const std::string&) { return std::string(R"({"say":"For the Mist!","actions":[{"type":"end_turn"}]})"); };
    {
        auto g = makeGame(true, dc);
        auto table = std::make_shared<AgentTable>(*cfg, factory);
        AgentTable::Attach(table, *g);
        int guard = 0;
        while (g->GetMode() == Mode::Combat && guard++ < 500) g->AdvanceCombat();
        EXPECT_NE(g->GetMode(), Mode::Combat);
        EXPECT_TRUE(minds.count("Thorin") && minds.count("Elara") && minds.count("DM"));
        EXPECT_TRUE(minds["Thorin"].calls > 0 && minds["Elara"].calls > 0);
        EXPECT_TRUE(minds["Thorin"].model == "dolphin-llama3:8b" && minds["Elara"].model == "qwen2.5:7b" && minds["Finn"].model == "phi3:mini");
        EXPECT_TRUE(minds["Thorin"].system.find("gruff") != std::string::npos && minds["Thorin"].system.find("curious") == std::string::npos);
        EXPECT_TRUE(minds["Elara"].system.find("curious") != std::string::npos && minds["Elara"].system.find("gruff") == std::string::npos);
        bool said = false;
        for (const auto& m : table->Hub().LogJson(0)) said = said || (m["from"] == "Thorin" && m["kind"] == "say");
        EXPECT_TRUE(said);
        bool inLog = false;
        for (const auto& e : g->Log()) inLog = inLog || e.text.find("Thorin: \"For the Mist!\"") != std::string::npos;
        EXPECT_TRUE(inLog);
        EXPECT_TRUE(table->Describe().size() >= 5);
    }

    // ---- garbage from every model leaves the built-in AI in charge: same result as a plain run
    replyFn = [](const std::string&, const std::string&) { return std::string("no idea, sorry"); };
    {
        auto plain = makeGame(false, dc);
        int guard = 0;
        while (plain->GetMode() == Mode::Combat && guard++ < 500) plain->AdvanceCombat();
        auto g = makeGame(true, dc);
        auto table = std::make_shared<AgentTable>(*cfg, factory);
        AgentTable::Attach(table, *g);
        guard = 0;
        while (g->GetMode() == Mode::Combat && guard++ < 500) g->AdvanceCombat();
        EXPECT_TRUE(g->Save()["rng"] == plain->Save()["rng"] && g->Save()["party"] == plain->Save()["party"]);
    }

    // ---- each kind of enemy has its own mind, and its model can be switched (now and for later spawns)
    {
        minds.clear();
        auto g = makeGame(true, dc);
        auto table = std::make_shared<AgentTable>(*cfg, factory);
        AgentTable::Attach(table, *g);
        GameActor zombie;
        zombie.team = Team::Monsters;
        zombie.sheet.monsterId = "zombie";
        zombie.sheet.tags = {"undead"};
        GameActor wolf = GameActor{};
        wolf.team = Team::Monsters;
        wolf.sheet.monsterId = "wolf";
        wolf.sheet.tags = {"beast"};
        auto z1 = table->MonsterAgent(zombie), w1 = table->MonsterAgent(wolf);
        EXPECT_TRUE(z1 && w1 && z1 != w1 && z1->Def().model == "dolphin-llama3:8b" && w1->Def().model == "gemma2:2b");
        EXPECT_EQ(table->MonsterAgent(zombie), z1);  // one mind per kind of enemy
        EXPECT_TRUE(table->SetModel("type:undead", "phi3:mini", "", false, err));
        EXPECT_TRUE(z1->Def().model == "phi3:mini" && minds["Undead"].model == "phi3:mini" && minds["Undead"].made == 2);
        EXPECT_EQ(w1->Def().model, "gemma2:2b");  // the others are untouched
        EXPECT_TRUE(table->SetModel("type:giant", "qwen2.5:7b", "", false, err));  // not made yet: applies when the first giant appears
        GameActor ogre;
        ogre.team = Team::Monsters;
        ogre.sheet.monsterId = "ogre";
        ogre.sheet.tags = {"giant"};
        EXPECT_EQ(table->MonsterAgent(ogre)->Def().model, "qwen2.5:7b");
        EXPECT_TRUE(!table->SetModel("type:unicorn", "x", "", false, err) && err.find("unknown selector") != std::string::npos);
        EXPECT_TRUE(!table->SetModel("dm", "no-such-model-in-the-store", "", true, err) && err.find("model store") != std::string::npos);
        EXPECT_FALSE(table->SetModel("dm", "x", "nope", false, err));
        EXPECT_TRUE(table->SetModel("monster:goblin", "phi3:mini", "", false, err));
    }

    // ---- NPCs: typed, each with its own mind; the written line is the fallback
    {
        GameConfig mc;
        mc.moduleId = "ravenmoor"; mc.seed = 5; mc.role = Role::Player; mc.agents = true;
        replyFn = [](const std::string& who, const std::string&) {
            return who == "Marta Hollin" ? std::string(R"({"say":"Folk say the Countess walks at night."})") : std::string(R"({"actions":[{"type":"end_turn"}]})");
        };
        minds.clear();
        Game g(mc);
        auto table = std::make_shared<AgentTable>(*cfg, factory);
        AgentTable::Attach(table, g);
        EXPECT_TRUE(g.Travel("greywick").ok);
        EXPECT_TRUE(g.Talk("marta", "rumours").ok);
        EXPECT_TRUE(minds.count("Marta Hollin") && minds["Marta Hollin"].model == "phi3:mini");
        EXPECT_NE(minds["Marta Hollin"].system.find("innkeeper"), std::string::npos);
        bool spoke = false;
        for (const auto& e : g.Log()) spoke = spoke || e.text == "Marta Hollin: Folk say the Countess walks at night.";
        EXPECT_TRUE(spoke);

        replyFn = [](const std::string&, const std::string&) { return std::string("garbled"); };
        Game g2(mc);
        auto table2 = std::make_shared<AgentTable>(*cfg, factory);
        AgentTable::Attach(table2, g2);
        EXPECT_TRUE(g2.Travel("greywick").ok);
        const size_t before = g2.Log().size();
        EXPECT_TRUE(g2.Talk("marta", "rumours").ok);
        EXPECT_TRUE(g2.Log().size() > before && g2.Log().back().text.rfind("Marta Hollin: ", 0) == 0);
        EXPECT_EQ(g2.Log().back().text.find("Countess walks"), std::string::npos);  // the module's own text, not the mock's
    }

    // ---- the party decides where to go by talking it over
    {
        GameConfig mc;
        mc.moduleId = "ravenmoor"; mc.seed = 5; mc.role = Role::Spectator; mc.agents = true;
        replyFn = [](const std::string& who, const std::string&) {
            if (who == "Finn") return std::string(R"({"say":"Search first?","propose":{"cmd":"search"}})");
            return std::string(R"({"say":"To the village.","propose":{"cmd":"travel","to":"greywick"}})");
        };
        Game g(mc);
        auto table = std::make_shared<AgentTable>(*cfg, factory);
        AgentTable::Attach(table, g);
        EXPECT_TRUE(g.AutoStep().ok);
        EXPECT_EQ(g.LocationId(), "greywick");  // three votes beat one
    }
}
