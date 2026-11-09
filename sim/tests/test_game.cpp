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


TEST(Game, RemoteCombat) {
    // A RemoteController pauses the engine; Submit resumes it. Saving while paused and loading elsewhere continues identically.
    auto run = [](bool saveAndReload) {
        GameConfig cfg;
        cfg.rooms = 1;
        cfg.difficulty = "easy";
        cfg.seed = 11;
        cfg.role = Role::Player;
        cfg.hero = "elara";
        auto g = std::make_unique<Game>(cfg);
        EXPECT_EQ(g->GetMode(), Mode::Combat);
        int prompts = 0;
        for (int guard = 0; guard < 400 && g->GetMode() == Mode::Combat; ++guard) {
            if (g->AdvanceCombat() == StepStatus::AwaitingInput) {
                ++prompts;
                if (saveAndReload && prompts == 2) g = Game::Load(g->Save());
                Action a;  // fire bolt at the nearest living monster, else just end the turn
                const Combat& cb = *g->GetCombat();
                const GameActor* me = cb.Find(cb.AwaitingActor());
                a.type = ActionType::EndTurn;
                for (const auto* h : cb.HostilesOf(*me, false)) {
                    a.type = ActionType::CastSpell; a.spellId = "fire_bolt"; a.targetId = h->id; break;
                }
                g->SubmitAction(a);
                if (g->GetMode() == Mode::Combat && g->GetCombat()->AwaitingActor() >= 0) {
                    Action e; e.type = ActionType::EndTurn; g->SubmitAction(e);
                }
            }
        }
        EXPECT_TRUE(prompts >= 1);
        EXPECT_NE(g->GetMode(), Mode::Combat);
        return g->Save().dump();
    };
    EXPECT_EQ(run(false), run(true));
}

TEST(Game, GameModule) {
    GameConfig cfg;
    cfg.moduleId = "ravenmoor";
    cfg.seed = 21;
    cfg.role = Role::Spectator;
    Game g(cfg);
    EXPECT_TRUE(g.GetMode() == Mode::Exploring && g.LocationId() == Content::FindModule("ravenmoor")->start);
    EXPECT_TRUE(g.HasFlag("relic") && (g.FlagValue("relic") == "old_chapel" || g.FlagValue("relic") == "standing_stones"));
    EXPECT_FALSE(g.Travel("nowhere").ok);

    EXPECT_TRUE(g.Travel("greywick").ok || g.GetMode() == Mode::Combat);
    while (g.GetMode() == Mode::Combat) g.AutoStep();
    EXPECT_EQ(g.LocationId(), "greywick");
    EXPECT_FALSE(g.Travel("crypt").ok);                      // blocked until the silver key is found
    EXPECT_TRUE(g.Rest("long").ok || g.GetMode() == Mode::Combat);
    while (g.GetMode() == Mode::Combat) g.AutoStep();
    const auto again = g.Rest("long");
    EXPECT_FALSE(again.ok);                                   // SRD: one long rest per 24 hours
    EXPECT_FALSE(g.Talk("marta", "no_such_topic").ok);
    EXPECT_TRUE(g.Talk("marta", "rumours").ok && g.HasFlag("heard_of_key"));

    // Autopilot clears the whole module on most seeds and always terminates.
    int done = 0;
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
        GameConfig c = cfg;
        c.seed = seed;
        Game x(c);
        int steps = 0;
        while ((x.GetMode() == Mode::Exploring || x.GetMode() == Mode::Combat) && steps++ < 4000)
            if (!x.AutoStep().ok) break;
        EXPECT_TRUE(x.GetMode() == Mode::Complete || x.GetMode() == Mode::Defeated);
        done += x.GetMode() == Mode::Complete;
        if (x.GetMode() == Mode::Complete) EXPECT_TRUE(x.HasFlag("countess_defeated") && x.HasFlag("has_silver_key"));
    }
    EXPECT_TRUE(done >= 3);
}

TEST(Game, SaveLoadDeterminism) {
    GameConfig cfg;
    cfg.moduleId = "ravenmoor";
    cfg.seed = 5;
    cfg.role = Role::Spectator;
    Game a(cfg);
    for (int i = 0; i < 18; ++i) a.AutoStep();
    auto b = Game::Load(a.Save());
    EXPECT_EQ(a.Save(), b->Save());
    for (int i = 0; i < 300 && a.GetMode() != Mode::Complete && a.GetMode() != Mode::Defeated; ++i) {
        a.AutoStep();
        b->AutoStep();
    }
    EXPECT_EQ(a.Save().dump(), b->Save().dump());
    // Same seed, same story.
    Game c(cfg);
    for (int i = 0; i < 18; ++i) c.AutoStep();
    Game d(cfg);
    for (int i = 0; i < 18; ++i) d.AutoStep();
    EXPECT_EQ(c.Save().dump(), d.Save().dump());
}

TEST(Game, ProceduralDelve) {
    for (std::uint64_t seed = 1; seed <= 12; ++seed) {
        GameConfig cfg;
        cfg.rooms = 2;
        cfg.difficulty = seed % 2 ? "easy" : "medium";
        cfg.seed = seed;
        cfg.role = Role::Spectator;
        Game g(cfg);
        int steps = 0;
        while ((g.GetMode() == Mode::Exploring || g.GetMode() == Mode::Combat) && steps++ < 2000)
            if (!g.AutoStep().ok) break;
        EXPECT_TRUE(g.GetMode() == Mode::Complete || g.GetMode() == Mode::Defeated);
    }
}

TEST(Game, RoleSwap) {
    // Combat::Run style: the scripted human swaps to DM, then input ends and the AI takes over.
    std::istringstream in("swap dm\n");
    std::ostringstream out;
    GameConfig cfg;
    cfg.rooms = 1;
    cfg.difficulty = "easy";
    cfg.seed = 99;
    cfg.role = Role::Player;
    Game g(cfg);
    g.SetHumanFactory([&] { return std::make_unique<HumanController>(in, out); });
    std::string log;
    g.SetListener([&](const Event& e) { log += e.text + "\n"; });
    int guard = 0;
    while (g.GetMode() == Mode::Combat && guard++ < 200) g.AdvanceCombat();
    EXPECT_NE(g.GetMode(), Mode::Combat);
    EXPECT_NE(log.find("[role is now: dungeon master]"), std::string::npos);
    EXPECT_NE(log.find("[role is now: spectator]"), std::string::npos);
}

TEST(Game, HeroSwitch) {
    GameConfig cfg;
    cfg.rooms = 1; cfg.difficulty = "easy"; cfg.seed = 11; cfg.role = Role::Player; cfg.hero = "elara";
    Game g(cfg);
    EXPECT_TRUE(g.GetMode() == Mode::Combat && g.Hero() == "Elara");
    std::string err;
    EXPECT_TRUE(!g.SetHero("nobody", err) && err.find("nobody") != std::string::npos);
    std::set<std::string> asked;  // whose input the engine waits for
    bool switched = false;
    for (int guard = 0; guard < 600 && g.GetMode() == Mode::Combat; ++guard) {
        if (g.AdvanceCombat() != StepStatus::AwaitingInput) continue;
        const Combat& cb = *g.GetCombat();
        const std::string who = cb.Find(cb.AwaitingActor())->Name();
        asked.insert(who);
        if (!switched) {  // hand the controls to Thorin while it is Elara's turn: the engine must now ask for Thorin
            EXPECT_TRUE(g.SetHero("thorin", err));
            switched = true;
            EXPECT_TRUE(g.Hero() == "Thorin" && g.GetRole() == Role::Player);
            continue;
        }
        Action e; e.type = ActionType::EndTurn;
        g.SubmitAction(e);
    }
    EXPECT_TRUE(switched && asked.count("Elara") == 1 && asked.count("Thorin") == 1 && asked.size() == 2);
    EXPECT_NE(g.GetMode(), Mode::Combat);

    // a spectator or DM who picks a hero becomes a player; the dead cannot be controlled
    GameConfig sc;
    sc.moduleId = "ravenmoor"; sc.seed = 2; sc.role = Role::Spectator;
    Game sg(sc);
    EXPECT_TRUE(sg.SetHero("finn", err) && sg.GetRole() == Role::Player && sg.Hero() == "Finn");
    EXPECT_EQ(Game::Load(sg.Save())->Hero(), "Finn");
}
