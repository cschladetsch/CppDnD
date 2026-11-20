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
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

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

TEST(Llm, NarratorPromptFollowsTheModuleSetting) {
    GameConfig cfg;
    cfg.moduleId = "cyberhood";
    cfg.seed = 2;
    Game g(cfg);
    std::string system;
    auto mock = std::make_shared<MockLlmClient>([&system](const std::string& sys, const std::string&, bool) {
        system = sys;
        LlmReply r;
        r.ok = true;
        r.content = "Neon hums overhead.";
        return r;
    });
    const auto narrate = MakeLlmNarrator(mock, g);
    Event e;
    e.type = EventType::Scene;
    e.text = "The doors hiss shut.";
    EXPECT_EQ(narrate(e), "Neon hums overhead.");
    EXPECT_NE(system.find("cyberpunk"), std::string::npos) << system;
    EXPECT_EQ(system.find("gothic"), std::string::npos) << system;
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

TEST(Llm, ParsesOllamaNdjson) {
    std::string err;
    const std::string raw = "{\"message\":{\"role\":\"assistant\",\"content\":\"Hel\"},\"done\":false}\n"
                            "\n{\"message\":{\"content\":\"lo\"},\"done\":false}\n"
                            "{\"done\":true}\n";
    EXPECT_EQ(ParseNdjsonChat(raw, err), "Hello");
    EXPECT_TRUE(err.empty());
    EXPECT_EQ(ParseNdjsonChat("{\"message\":{\"content\":\"a\"}}\n{\"error\":\"Ollama returned HTTP 404\"}\n", err), "a");
    EXPECT_EQ(err, "Ollama returned HTTP 404");
}

TEST(Llm, CppCoderBackendFormat) {
    std::string err;
    const auto cfg = AgentsConfig::Parse(Json::parse(R"({"backends":{"cc":{"url":"http://127.0.0.1:8765","format":"cppcoder"}},
        "defaults":{"backend":"cc","model":"m"}})"), err);
    ASSERT_TRUE(cfg) << err;
    EXPECT_EQ(cfg->FindBackend("cc")->path, "/api/chat");
    AgentDef d;
    d.backend = "cc";
    d.model = "m";
    const LlmConfig lc = cfg->ToLlmConfig(d);
    EXPECT_EQ(lc.format, "cppcoder");
    EXPECT_EQ(lc.path, "/api/chat");
}

TEST(Llm, CppCoderClientTalksToTheChatEndpoint) {
    httplib::Server svr;
    std::string seenModel, seenSystem, seenUser;
    svr.Post("/api/chat", [&](const httplib::Request& req, httplib::Response& res) {
        const auto j = nlohmann::json::parse(req.body);
        seenModel = j["model"];
        seenSystem = j["messages"][0]["content"];
        seenUser = j["messages"][1]["content"];
        res.set_content("{\"message\":{\"content\":\"{\\\"actions\\\":\"},\"done\":false}\n{\"message\":{\"content\":\"[]}\"},\"done\":true}\n", "application/x-ndjson");
    });
    const int port = svr.bind_to_any_port("127.0.0.1");
    std::thread t([&] { svr.listen_after_bind(); });
    svr.wait_until_ready();
    LlmConfig lc;
    lc.baseUrl = "http://127.0.0.1:" + std::to_string(port);
    lc.path = "/api/chat";
    lc.format = "cppcoder";
    lc.model = "qwen2.5:7b";
    HttpLlmClient client(lc);
    const LlmReply r = client.Chat("sys", "usr", true);
    svr.stop();
    t.join();
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.content, "{\"actions\":[]}");
    EXPECT_EQ(seenModel, "qwen2.5:7b");
    EXPECT_EQ(seenSystem, "sys");
    EXPECT_EQ(seenUser, "usr");
}

// ---------------------------------------------------------------- NPC lines read the NPC's own memory

namespace {
std::unique_ptr<Game> MeridianMess() {
    GameConfig cfg;
    cfg.moduleId = "meridian";
    cfg.seed = 3;
    cfg.role = Role::Spectator;
    Game g(cfg);
    Json j = g.Save();
    j["location"] = "promenade";
    return Game::Load(j);
}
}  // namespace

TEST(NpcLines, ChatterPromptCarriesWhatTheSpeakerRemembersOfTheListener) {
    auto g = MeridianMess();
    std::vector<std::string> prompts;
    auto mock = std::make_shared<MockLlmClient>([&](const std::string& system, const std::string& user, bool) {
        prompts.push_back(system + "\n" + user);
        LlmReply r;
        r.ok = true;
        r.content = "\"The rice is low.\"\n";
        return r;
    });
    g->SetChatter(MakeLlmChatter(mock, *g));
    ASSERT_TRUE(g->Travel("mess_hall").ok);
    g->Wait(1);
    ASSERT_GE(prompts.size(), 2u);
    EXPECT_NE(prompts[0].find("were together in"), std::string::npos);  // first line: all it remembers is seeing the other there
    EXPECT_NE(prompts[1].find("The rice is low."), std::string::npos);   // second line: the first one is now remembered
    EXPECT_NE(prompts[1].find("non-player character"), std::string::npos);
    EXPECT_EQ(prompts[1].find('\xE2'), std::string::npos);                 // nothing fancy in the prompt (no dashes)
    bool kept = false;
    const Json p = g->Hist().Pair("Quartermaster Bo Tran", "Cook Hob Marsh");
    for (const auto& e : p["entries"]) kept = kept || e["text"] == "The rice is low.";  // quotes and newline stripped
    EXPECT_TRUE(kept);
}

TEST(NpcLines, TheVoiceRemembersThePartyAndFallsBackWhenTheModelFails) {
    auto g = MeridianMess();
    ASSERT_TRUE(g->Travel("mess_hall").ok);
    std::string seen;
    auto mock = std::make_shared<MockLlmClient>([&](const std::string&, const std::string& user, bool) {
        seen = user;
        LlmReply r;
        r.ok = true;
        r.content = "Hob and I count the stores twice. Eleven of us eat here, and we are glad of company.";
        return r;
    });
    g->SetNpcVoice(MakeLlmNpcVoice(mock, *g));
    ASSERT_TRUE(g->Talk("tran", "who").ok);
    EXPECT_NE(seen.find("came into"), std::string::npos);  // it remembers the party arriving
    auto failing = std::make_shared<MockLlmClient>([](const std::string&, const std::string&, bool) { LlmReply r; r.error = "boom"; return r; });
    g->SetNpcVoice(MakeLlmNpcVoice(failing, *g));
    ASSERT_TRUE(g->Talk("tran", "stores").ok);  // the written text is used instead
}

TEST(Llm, MatureContentPolicy) {
    // ---- the preamble: empty when off; when on it drops refusals and always carries the adults-only limit
    EXPECT_TRUE(ContentPreamble(false).empty());
    const std::string pre = ContentPreamble(true);
    EXPECT_NE(pre.find("Do not refuse"), std::string::npos);
    EXPECT_NE(pre.find("18 or older"), std::string::npos);
    EXPECT_NE(pre.find("minor"), std::string::npos);
    EXPECT_NE(pre.find("always does what the player says"), std::string::npos);

    // ---- the wrapper puts it in front of every system prompt and leaves the rest alone
    std::string seen;
    auto inner = std::make_shared<MockLlmClient>([&](const std::string& sys, const std::string&, bool) { seen = sys; LlmReply r; r.ok = true; r.content = "{}"; return r; });
    EXPECT_EQ(ApplyContentPolicy(inner, false), inner);
    EXPECT_EQ(ApplyContentPolicy(nullptr, true), nullptr);
    auto wrapped = ApplyContentPolicy(inner, true);
    ASSERT_TRUE(std::dynamic_pointer_cast<ContentPolicyClient>(wrapped));
    wrapped->Chat("You are the narrator.", "hi", false);
    EXPECT_EQ(seen, pre + "You are the narrator.");
    EXPECT_NE(wrapped->Describe().find("content=mature"), std::string::npos);

    // ---- "act": a free-form action with no rules effect, narrated by the reply
    GameConfig cfg; cfg.moduleId = "ravenmoor"; cfg.seed = 11;
    Game g(cfg);
    const std::string before = g.LocationId();
    std::string sayPrompt;
    MockLlmClient actor([&](const std::string& sys, const std::string&, bool json) {
        LlmReply r; r.ok = true;
        if (!json) { r.content = "Elara laughs and hugs you back."; return r; }  // the narrator
        sayPrompt = sys;
        r.content = R"({"steps":[{"cmd":"act","what":"hug Elara"}],"reply":"Hm."})";
        return r;
    });
    const SayResult sr = Say(actor, g, "hug elara");
    EXPECT_TRUE(sr.ok);
    EXPECT_EQ(sr.steps, std::vector<std::string>{"act"});
    EXPECT_EQ(sr.message, "Elara laughs and hugs you back.");
    EXPECT_EQ(g.LocationId(), before);
    EXPECT_NE(sayPrompt.find("\"cmd\":\"act\""), std::string::npos);
    EXPECT_TRUE(RunExplorationStep(g, Json{{"cmd", "act"}}).ok);

    // ---- the agents file: content.mature wraps every agent; content.model is each actor's mature model unless it sets its own
    std::string err;
    auto ac = AgentsConfig::Parse(Json::parse(R"({"content":{"mature":true,"model":"uncensored"},
        "defaults":{"model":"aligned"},"players":{"finn":{"matureModel":"finn-mature"}}})"), err);
    ASSERT_TRUE(ac.has_value()) << err;
    EXPECT_TRUE(ac->mature);
    EXPECT_EQ(ac->ForPlayer("thorin", "Thorin").matureModel, "uncensored");
    EXPECT_EQ(ac->ForPlayer("finn", "Finn").matureModel, "finn-mature");
    EXPECT_TRUE(!AgentsConfig::Parse(Json::parse(R"({"defaults":{"model":"m"},"content":true})"), err) && err.find("content") != std::string::npos);
    std::string madeWith;
    AgentTable table(*ac, [&](const std::string&, const BackendDef&, const AgentDef& d) { madeWith = d.model; return std::make_shared<MockLlmClient>(); });
    auto dm = table.Dm();
    ASSERT_TRUE(dm);
    EXPECT_EQ(madeWith, "uncensored");
    EXPECT_EQ(dm->Def().model, "uncensored");
    EXPECT_TRUE(std::dynamic_pointer_cast<ContentPolicyClient>(dm->Client()));
    ASSERT_TRUE(table.SetModel("dm", "picked", "", false, err)) << err;  // an explicit switch wins over the mature model
    EXPECT_EQ(table.Dm()->Def().model, "picked");

    // ---- off by default, including the shipped agents.json
    auto off = AgentsConfig::Parse(Json::parse(R"({"defaults":{"model":"aligned"}})"), err);
    ASSERT_TRUE(off.has_value());
    EXPECT_FALSE(off->mature);
    AgentTable plain(*off, [](const std::string&, const BackendDef&, const AgentDef&) { return std::make_shared<MockLlmClient>(); });
    EXPECT_FALSE(std::dynamic_pointer_cast<ContentPolicyClient>(plain.Dm()->Client()));
}

TEST(Llm, SayNarratesStepsTheEngineHasNoCommandFor) {
    GameConfig cfg; cfg.moduleId = "ravenmoor"; cfg.seed = 11;
    Game g(cfg);
    const std::string before = g.LocationId();
    // a step with no cmd at all (what the model sent for "kade takes off her top and kneels"), and no reply: a second call narrates it
    int calls = 0;
    std::string narrateUser;
    MockLlmClient m([&](const std::string&, const std::string& user, bool json) {
        ++calls;
        LlmReply r; r.ok = true;
        if (json) r.content = R"({"steps":[{"action":"Kade kneels"},{"cmd":"emote","what":"smiles"}],"reply":""})";
        else { narrateUser = user; r.content = "Kade kneels on the wet tiles."; }
        return r;
    });
    const SayResult sr = Say(m, g, "kade kneels");
    EXPECT_TRUE(sr.ok);
    EXPECT_EQ(sr.steps, (std::vector<std::string>{"act", "act"}));
    EXPECT_EQ(sr.message, "Kade kneels on the wet tiles.");
    EXPECT_EQ(calls, 2);
    EXPECT_NE(narrateUser.find("Kade kneels; smiles"), std::string::npos);
    EXPECT_EQ(g.LocationId(), before);
    // an act step mixed with a real command still runs the command, and a given reply is not re-narrated
    // an act step mixed with a real command still runs the command; the narrator, not the translator's reply, describes the act
    std::string narratorSys;
    MockLlmClient mixed([&](const std::string& sys, const std::string&, bool json) {
        LlmReply r; r.ok = true;
        if (json) r.content = R"({"steps":[{"cmd":"act","what":"waves"},{"cmd":"search"}],"reply":"The constable frowns."})";
        else { narratorSys = sys; r.content = "You wave, then look around."; }
        return r; });
    const SayResult mx = Say(mixed, g, "wave and search");
    EXPECT_TRUE(mx.ok);
    EXPECT_EQ(mx.steps, (std::vector<std::string>{"act", "search"}));
    EXPECT_EQ(mx.message, "You wave, then look around.");
    EXPECT_NE(narratorSys.find("not one of the characters"), std::string::npos);
    EXPECT_NE(narratorSys.find(g.Hero()), std::string::npos);
    // the narrator failing keeps the translator's reply
    MockLlmClient noNarrator([&](const std::string&, const std::string&, bool json) {
        LlmReply r; r.ok = json; r.content = R"({"steps":[{"cmd":"act"}],"reply":"You wave."})"; return r; });
    EXPECT_EQ(Say(noNarrator, g, "wave").message, "You wave.");
    // plain prose instead of JSON is shown as the narration; broken JSON names what came back
    MockLlmClient prose([](const std::string&, const std::string&, bool) { LlmReply r; r.ok = true; r.content = "She kneels."; return r; });
    const SayResult pr = Say(prose, g, "kneel");
    EXPECT_TRUE(pr.ok && pr.message == "She kneels.");
    MockLlmClient broken([](const std::string&, const std::string&, bool) { LlmReply r; r.ok = true; r.content = "{\"steps\": [oops"; return r; });
    const SayResult br = Say(broken, g, "kneel");
    EXPECT_FALSE(br.ok);
    EXPECT_NE(br.message.find("not the JSON"), std::string::npos);
    EXPECT_NE(br.message.find("oops"), std::string::npos);
    EXPECT_NE(RunExplorationStep(g, Json::object()).message.find("no command"), std::string::npos);
}
