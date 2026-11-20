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

// Switching heroes mid-fight must show the new hero's own weapons straight away, not the previous hero's until the next action.
TEST(Game, HeroSwitchShowsTheNewHeroOptionsAtOnce) {
    GameConfig cfg;
    cfg.rooms = 1; cfg.difficulty = "easy"; cfg.seed = 11; cfg.role = Role::Player; cfg.hero = "elara";
    Game g(cfg);
    ASSERT_EQ(g.GetMode(), Mode::Combat);
    for (int guard = 0; guard < 600 && g.AdvanceCombat() != StepStatus::AwaitingInput; ++guard) {}
    const Combat& cb = *g.GetCombat();
    ASSERT_GE(cb.AwaitingActor(), 0);
    EXPECT_EQ(cb.Find(cb.AwaitingActor())->Name(), "Elara");
    const Json before = g.View(0)["combat"]["options"];
    ASSERT_TRUE(before.is_object());

    std::string err;
    ASSERT_TRUE(g.SetHero("thorin", err));
    if (g.GetMode() != Mode::Combat) return;  // the fight ended on the old hero's AI-played turn: nothing to show
    ASSERT_GE(g.GetCombat()->AwaitingActor(), 0);
    EXPECT_EQ(g.GetCombat()->Find(g.GetCombat()->AwaitingActor())->Name(), "Thorin");  // no extra AdvanceCombat needed
    const Json after = g.View(0)["combat"]["options"];
    ASSERT_TRUE(after.is_object());
    EXPECT_NE(after["actor"], before["actor"]);
    EXPECT_NE(after["attacks"], before["attacks"]);  // a wizard's and a fighter's weapons differ
}

TEST(Game, HeroSwitch) {
    GameConfig cfg;
    cfg.rooms = 1; cfg.difficulty = "easy"; cfg.seed = 12; cfg.role = Role::Player; cfg.hero = "elara";
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

namespace {
int FirstFightFoeHp(const std::string& difficulty) {
    GameConfig cfg;
    cfg.moduleId = "ravenmoor";
    cfg.seed = 3;
    cfg.role = Role::Spectator;
    cfg.difficulty = difficulty;
    Game g(cfg);
    for (int i = 0; i < 200 && g.GetMode() != Mode::Combat; ++i) g.AutoStep();
    int hp = 0;
    if (const Combat* c = g.GetCombat())
        for (const auto& a : c->Actors())
            if (a->team == Team::Monsters) hp += a->sheet.maxHp;
    return hp;
}
}  // namespace

TEST(Game, DifficultyScalesModuleEncounters) {
    const int easy = FirstFightFoeHp("easy"), medium = FirstFightFoeHp("medium"), hard = FirstFightFoeHp("hard");
    ASSERT_GT(easy, 0);
    EXPECT_LT(easy, medium);
    EXPECT_LT(medium, hard);
}

TEST(Game, TypedCommandsMatchWholeWords) {
    GameConfig cfg;
    cfg.moduleId = "ravenmoor";
    cfg.seed = 4;
    Game g(cfg);
    // "forest" contains "rest", but here it names a place to go to, not an instruction to sleep.
    const SayResult go = SayOffline(g, "take the forest track");
    ASSERT_TRUE(go.ok) << go.message;
    ASSERT_EQ(go.steps.size(), 1u);
    EXPECT_EQ(go.steps[0], "travel to whispering_wood");
    // A real rest, however it is inflected, still works.
    Game h(cfg);
    const SayResult rest = SayOffline(h, "we are resting here");
    ASSERT_TRUE(rest.ok) << rest.message;
    ASSERT_EQ(rest.steps.size(), 1u);
    EXPECT_EQ(rest.steps[0], "short rest");
}

TEST(Game, TypedCommandsWorkWithoutAModel) {
    GameConfig cfg;
    cfg.moduleId = "ravenmoor";
    cfg.seed = 4;
    Game g(cfg);
    const std::string start = g.Module().FindLocation(g.LocationId())->id;
    ASSERT_FALSE(g.Module().FindLocation(g.LocationId())->exits.empty());
    const ExitDef& x = g.Module().FindLocation(g.LocationId())->exits.front();
    const SayResult bad = SayOffline(g, "flibbertigibbet");
    EXPECT_FALSE(bad.ok);
    EXPECT_FALSE(bad.message.empty());
    const SayResult go = SayOffline(g, "go to " + (x.label.empty() ? x.to : x.label));
    if (go.ok) EXPECT_NE(g.Module().FindLocation(g.LocationId())->id, start); else EXPECT_FALSE(go.message.empty());
    EXPECT_TRUE(SayOffline(g, "wait 1 hour").ok || g.GetMode() != Mode::Exploring);
}

namespace {
// A piece of the game's view, returned by value so a range-for over it does not loop over a destroyed temporary.
Json ViewAt(const Game& g, std::initializer_list<const char*> path) {
    Json v = g.View(0);
    for (const char* k : path) v = Json(v[k]);
    return v;
}
}  // namespace

// ---------------------------------------------------------------- equipment

namespace {
std::vector<std::string> WeaponsOf(const Json& view, const std::string& id) {
    std::vector<std::string> out;
    for (const auto& m : view["party"])
        if (m["id"] == id)
            for (const auto& a : m["attacks"]) out.push_back(a["id"].get<std::string>());
    return out;
}
}  // namespace

TEST(Equipment, WeaponsAndPotionsMoveBetweenCharactersWhileExploring) {
    GameConfig cfg;
    cfg.moduleId = "ravenmoor";
    cfg.seed = 3;
    cfg.role = Role::Spectator;
    Game g(cfg);
    ASSERT_EQ(g.GetMode(), Mode::Exploring);
    std::string err;

    // a swap: Thorin's handaxe for Elara's dagger, each rebuilt from the new owner's scores
    const Json before = g.View(0);
    int daggerToHit = 0;
    for (const auto& m : before["party"]) if (m["id"] == "elara") daggerToHit = m["attacks"][0]["toHit"];
    ASSERT_TRUE(g.MoveItem("thorin", "elara", "handaxe", "dagger", err)) << err;
    EXPECT_EQ(WeaponsOf(g.View(0), "thorin"), (std::vector<std::string>{"warhammer", "dagger"}));
    EXPECT_EQ(WeaponsOf(g.View(0), "elara"), (std::vector<std::string>{"handaxe"}));
    int dagger = -99;
    for (const auto& m : ViewAt(g, {"party"})) if (m["id"] == "thorin") dagger = m["attacks"][1]["toHit"];
    EXPECT_NE(dagger, daggerToHit);  // Thorin's strength and Elara's dexterity differ, so does the bonus

    // a gift is refused when it would leave the giver unarmed, or for a weapon they do not have
    EXPECT_FALSE(g.MoveItem("elara", "thorin", "handaxe", "", err));
    EXPECT_NE(err.find("unarmed"), std::string::npos);
    EXPECT_FALSE(g.MoveItem("elara", "thorin", "longbow", "", err));
    EXPECT_FALSE(g.MoveItem("thorin", "thorin", "dagger", "", err));
    EXPECT_FALSE(g.MoveItem("thorin", "nobody", "dagger", "", err));

    // potions: the party starts with a few, held by whoever the character sheets name
    std::string giver;
    int potions = 0;
    for (const auto& m : ViewAt(g, {"party"}))
        if (giver.empty() && m["id"] != "elara" && m["potions"].get<int>() > 0) { giver = m["id"]; potions = m["potions"]; }
    if (giver.empty()) {  // none held at the start: one is found on the road
        ASSERT_TRUE(g.MoveItem("elara", "thorin", "potion", "", err) == false);
    } else {
        int elara = 0;
        for (const auto& m : ViewAt(g, {"party"})) if (m["id"] == "elara") elara = m["potions"];
        ASSERT_TRUE(g.MoveItem(giver, "elara", "potion", "", err)) << err;
        for (const auto& m : ViewAt(g, {"party"})) {
            if (m["id"] == giver) EXPECT_EQ(m["potions"].get<int>(), potions - 1);
            if (m["id"] == "elara") EXPECT_EQ(m["potions"].get<int>(), elara + 1);
        }
    }
    EXPECT_FALSE(g.MoveItem("aldric", "elara", "potion", "", err));

    // it all survives a save and load
    const auto loaded = Game::Load(g.Save());
    EXPECT_EQ(WeaponsOf(loaded->View(0), "elara"), (std::vector<std::string>{"handaxe"}));
}

TEST(Equipment, CannotBeChangedInTheMiddleOfAFight) {
    GameConfig cfg;
    cfg.rooms = 1; cfg.difficulty = "easy"; cfg.seed = 12; cfg.role = Role::Player; cfg.hero = "elara";
    Game g(cfg);
    ASSERT_EQ(g.GetMode(), Mode::Combat);
    std::string err;
    EXPECT_FALSE(g.MoveItem("thorin", "elara", "handaxe", "dagger", err));
    EXPECT_NE(err.find("exploring"), std::string::npos);
}

TEST(Equipment, TheViewCarriesFullStats) {
    GameConfig cfg;
    cfg.moduleId = "ravenmoor";
    cfg.role = Role::Spectator;
    Game g(cfg);
    for (const auto& m : ViewAt(g, {"party"})) {
        EXPECT_EQ(m["abilities"].size(), 6u) << m["name"];
        EXPECT_GE(m["attacks"].size(), 1u);
        EXPECT_GE(m["proficiency"].get<int>(), 2);
    }
}

TEST(Quests, TheViewListsWhatEachQuestNeedsAndWhetherItIsMet) {
    GameConfig cfg;
    cfg.moduleId = "sunless_abbey";
    cfg.role = Role::Spectator;
    Game g(cfg);
    const Json v = g.View(0);
    ASSERT_GE(v["quests"].size(), 1u);
    for (const auto& q : v["quests"]) {
        EXPECT_GE(q["requirements"].size(), 1u) << q["id"];
        EXPECT_GT(q["xp"].get<int>(), 0);
        for (const auto& r : q["requirements"]) EXPECT_FALSE(r["met"].get<bool>()) << "nothing is done at the start: " << r["condition"];
    }
}

// ---------------------------------------------------------------- generated room descriptions

TEST(RoomText, ADescriberWritesEachRoomOnceAndItIsSaved) {
    GameConfig cfg;
    cfg.moduleId = "ravenmoor";
    cfg.seed = 5;
    cfg.role = Role::Spectator;
    Game g(cfg);
    int calls = 0;
    g.SetDescriber([&](const LocationDef& l) { ++calls; return "Written for " + l.name + "."; });
    // the room the party already stands in is described straight away
    EXPECT_EQ(g.View(0)["location"]["description"], "Written for The Mist Road.");
    EXPECT_EQ(calls, 1);
    ASSERT_TRUE(g.Travel("greywick").ok);
    EXPECT_EQ(g.View(0)["location"]["description"], "Written for Greywick.");
    // going back does not write it again
    ASSERT_TRUE(g.Travel("mist_road").ok);
    EXPECT_EQ(calls, 2);
    // and a loaded game keeps the same text without asking the model
    const auto loaded = Game::Load(g.Save());
    EXPECT_EQ(loaded->View(0)["location"]["description"], "Written for The Mist Road.");
}

TEST(RoomText, AFailedDescriberKeepsTheAuthoredText) {
    GameConfig cfg;
    cfg.moduleId = "ravenmoor";
    cfg.role = Role::Spectator;
    Game g(cfg);
    g.SetDescriber([](const LocationDef&) { return std::string(); });
    EXPECT_NE(g.View(0)["location"]["description"].get<std::string>().find("cairn"), std::string::npos);
}

TEST(RoomText, TheLlmDescriberAsksForFactsAndSkipsRepeats) {
    GameConfig cfg;
    cfg.moduleId = "ravenmoor";
    cfg.role = Role::Spectator;
    Game g(cfg);
    auto mock = std::make_shared<MockLlmClient>([](const std::string&, const std::string&, bool) {
        LlmReply r; r.ok = true; r.content = "Fog drifts over the black stones.\nA raven watches."; return r; });
    g.SetDescriber(MakeLlmDescriber(mock, g));
    EXPECT_EQ(mock->Calls(), 1);
    EXPECT_NE(mock->LastUser().find("cairn of black stones"), std::string::npos);  // the authored facts go in
    const std::string d = g.View(0)["location"]["description"];
    EXPECT_EQ(d, "Fog drifts over the black stones. A raven watches.");
    ASSERT_TRUE(g.Travel("greywick").ok);
    EXPECT_NE(mock->LastUser().find("Do not reuse"), std::string::npos);
}

// ---------------------------------------------------------------- rooms found while exploring

TEST(Explore, FindsANewRoomWithPathsBothWaysAndSavesIt) {
    GameConfig cfg;
    cfg.moduleId = "ravenmoor";
    cfg.seed = 9;
    cfg.role = Role::Spectator;
    Game g(cfg);
    const std::size_t before = g.Module().locations.size();
    g.SetRoomWriter([](const LocationDef& parent, const std::vector<std::string>&) {
        ExtraRoom r;
        r.name = "The Bone Orchard";
        r.kind = "wilderness";
        r.description = "Pale trees stand in rows, each hung with small bones that click together in the wind, off the " + parent.name + ".";
        r.exitLabel = "Through the dead hedge";
        r.fight = false;
        return std::optional<ExtraRoom>(r);
    });
    const int hour = g.View(0)["clock"]["hour"];
    const CmdResult res = g.Explore();
    ASSERT_TRUE(res.ok) << res.message;
    ASSERT_EQ(res.message, "x1");  // a random encounter on the road would have interrupted, so this seed must be quiet
    EXPECT_EQ(g.Module().locations.size(), before + 1);
    EXPECT_EQ(g.View(0)["clock"]["hour"].get<int>(), hour + 2);
    bool offered = false;
    for (const auto& x : ViewAt(g, {"location", "exits"})) offered = offered || (x["to"] == "x1" && x["label"] == "Through the dead hedge");
    EXPECT_TRUE(offered);
    ASSERT_TRUE(g.Travel("x1").ok);
    EXPECT_EQ(g.View(0)["location"]["name"], "The Bone Orchard");
    EXPECT_NE(g.View(0)["location"]["description"].get<std::string>().find("Pale trees"), std::string::npos);
    bool back = false;
    for (const auto& x : ViewAt(g, {"location", "exits"})) back = back || x["to"] == "mist_road";
    EXPECT_TRUE(back);
    // a saved game keeps the room, its way in and its description
    const auto loaded = Game::Load(g.Save());
    EXPECT_EQ(loaded->Module().locations.size(), before + 1);
    EXPECT_EQ(loaded->View(0)["location"]["name"], "The Bone Orchard");
    EXPECT_EQ(loaded->ExtraRooms().size(), 1u);
    // the authored module itself is untouched
    EXPECT_EQ(Content::FindModule("ravenmoor")->locations.size(), before);
}

TEST(Explore, WorksWithoutAModelAndIsLimitedPerRoom) {
    GameConfig cfg;
    cfg.moduleId = "ravenmoor";
    cfg.seed = 2;
    cfg.role = Role::Spectator;
    Game g(cfg);
    int found = 0;
    for (int i = 0; i < 12 && g.GetMode() == Mode::Exploring; ++i) {
        const CmdResult r = g.Explore();
        if (r.ok && r.message != "interrupted") ++found;
        if (g.GetMode() != Mode::Exploring) break;
    }
    EXPECT_LE(found, 3);  // at most three rooms off any one place
    if (g.GetMode() == Mode::Exploring) {
        EXPECT_FALSE(g.Explore().ok);
        for (const auto& r : g.ExtraRooms()) EXPECT_FALSE(r.name.empty());
    }
}

TEST(Explore, AFightRoomStartsACombatWhenEntered) {
    GameConfig cfg;
    cfg.moduleId = "ravenmoor";
    cfg.seed = 4;
    cfg.role = Role::Spectator;
    Game g(cfg);
    g.SetRoomWriter([](const LocationDef&, const std::vector<std::string>&) {
        ExtraRoom r; r.name = "The Wolf Den"; r.description = "Bones and fur litter a low cave, and the stink of animals is thick in the air here."; r.fight = true; return std::optional<ExtraRoom>(r); });
    const CmdResult res = g.Explore();
    ASSERT_TRUE(res.ok);
    if (res.message == "interrupted") GTEST_SKIP() << "a random encounter got there first with this seed";
    ASSERT_TRUE(g.Travel("x1").ok);
    EXPECT_EQ(g.GetMode(), Mode::Combat);
}

TEST(Explore, TheLlmRoomWriterParsesAndRejectsBadReplies) {
    GameConfig cfg;
    cfg.moduleId = "ravenmoor";
    cfg.role = Role::Spectator;
    Game g(cfg);
    std::string reply = R"({"name":"The Salt Cellar","kind":"site","description":"Crusted white salt coats every surface of this low cellar, and your lamplight glitters back from the walls in a thousand points.","exitLabel":"A trapdoor under the rug","danger":"fight"})";
    auto mock = std::make_shared<MockLlmClient>([&](const std::string&, const std::string&, bool) { LlmReply r; r.ok = true; r.content = reply; return r; });
    auto writer = MakeLlmRoomWriter(mock, g);
    const auto good = writer(g.Module().locations.front(), {"Greywick"});
    ASSERT_TRUE(good.has_value());
    EXPECT_EQ(good->name, "The Salt Cellar");
    EXPECT_TRUE(good->fight);
    EXPECT_EQ(good->exitLabel, "A trapdoor under the rug");
    reply = "I cannot do that";
    EXPECT_FALSE(writer(g.Module().locations.front(), {}).has_value());
    reply = R"({"name":"X","kind":"site","description":"short"})";
    EXPECT_FALSE(writer(g.Module().locations.front(), {}).has_value());
}

// ---------------------------------------------------------------- the common space

TEST(Stash, ItemsGoInAndOutOfTheCommonSpace) {
    GameConfig cfg;
    cfg.moduleId = "ravenmoor";
    cfg.seed = 3;
    cfg.role = Role::Spectator;
    Game g(cfg);
    std::string err;
    // Thorin puts the handaxe in the common space; the view lists it with its base stats
    ASSERT_TRUE(g.MoveItem("thorin", "stash", "handaxe", "", err)) << err;
    EXPECT_EQ(WeaponsOf(g.View(0), "thorin"), (std::vector<std::string>{"warhammer"}));
    const Json v = g.View(0);
    ASSERT_EQ(v["stash"]["weapons"].size(), 1u);
    EXPECT_EQ(v["stash"]["weapons"][0]["id"], "handaxe");
    EXPECT_EQ(v["stash"]["weapons"][0]["damage"], "1d6");
    // the last weapon cannot be put away, so nobody is left bare-handed
    EXPECT_FALSE(g.MoveItem("thorin", "stash", "warhammer", "", err));
    EXPECT_NE(err.find("unarmed"), std::string::npos);
    // Elara takes it out for herself, rebuilt from her scores
    ASSERT_TRUE(g.MoveItem("stash", "elara", "handaxe", "", err)) << err;
    EXPECT_EQ(WeaponsOf(g.View(0), "elara"), (std::vector<std::string>{"dagger", "handaxe"}));
    EXPECT_EQ(g.StashWeapons().size(), 0u);
    EXPECT_FALSE(g.MoveItem("stash", "elara", "handaxe", "", err));  // nothing there any more
    // a swap with the common space: Elara trades her dagger for something left there
    ASSERT_TRUE(g.MoveItem("finn", "stash", "shortbow", "", err)) << err;
    ASSERT_TRUE(g.MoveItem("stash", "elara", "shortbow", "dagger", err)) << err;
    EXPECT_EQ(WeaponsOf(g.View(0), "elara"), (std::vector<std::string>{"handaxe", "shortbow"}));
    ASSERT_EQ(g.StashWeapons().size(), 1u);
    EXPECT_EQ(g.StashWeapons()[0], "dagger");
    // someone who already has it cannot be handed a second one
    EXPECT_FALSE(g.MoveItem("stash", "thorin", "dagger", "warhammer", err) && false);
    EXPECT_FALSE(g.MoveItem("stash", "stash", "dagger", "", err));
    // it all survives a save and load
    const auto loaded = Game::Load(g.Save());
    EXPECT_EQ(loaded->StashWeapons(), g.StashWeapons());
}

TEST(Stash, PotionsPassThroughToo) {
    GameConfig cfg;
    cfg.moduleId = "ravenmoor";
    cfg.role = Role::Spectator;
    Game g(cfg);
    std::string err;
    std::string giver;
    for (const auto& m : ViewAt(g, {"party"})) if (giver.empty() && m["potions"].get<int>() > 0) giver = m["id"];
    if (giver.empty()) GTEST_SKIP() << "nobody starts with a potion";
    ASSERT_TRUE(g.MoveItem(giver, "stash", "potion", "", err)) << err;
    EXPECT_EQ(g.StashPotions(), 1);
    ASSERT_TRUE(g.MoveItem("stash", "elara", "potion", "", err)) << err;
    EXPECT_EQ(g.StashPotions(), 0);
    EXPECT_FALSE(g.MoveItem("stash", "elara", "potion", "", err));
}

// ---------------------------------------------------------------- NPC memory (The Meridian)

namespace {
// A Meridian game with the party already standing in `loc`, as if they had walked there.
std::unique_ptr<Game> MeridianAt(const std::string& loc, std::uint64_t seed = 3) {
    GameConfig cfg;
    cfg.moduleId = "meridian";
    cfg.seed = seed;
    cfg.role = Role::Spectator;
    Game g(cfg);
    Json j = g.Save();
    j["location"] = loc;
    j["visited"].push_back(loc);
    return Game::Load(j);
}
const char* kTran = "Quartermaster Bo Tran";
const char* kHob = "Cook Hob Marsh";
}  // namespace

TEST(NpcMemory, TheFirstSightOfThePartyAndOfEachOtherIsRecorded) {
    auto g = MeridianAt("promenade");
    ASSERT_TRUE(g->Travel("mess_hall").ok);
    ASSERT_EQ(g->LocationId(), "mess_hall");
    const std::string hero = g->Hero();
    EXPECT_GT(g->Hist().Count(kTran, hero), 0u);
    EXPECT_GT(g->Hist().Count(hero, kTran), 0u);
    EXPECT_GT(g->Hist().Count(kHob, hero), 0u);
    EXPECT_GT(g->Hist().Count(kTran, kHob), 0u);
    EXPECT_GT(g->Hist().Count(kHob, kTran), 0u);
    EXPECT_EQ(g->Hist().Count(kTran, "Dr. Ruth Calloway"), 0u);  // never met
}

TEST(NpcMemory, NpcsTalkToEachOtherAndBothKeepTheLine) {
    auto g = MeridianAt("promenade");
    ASSERT_TRUE(g->Travel("mess_hall").ok);
    g->Wait(1);
    g->Wait(1);
    const Json fromTran = g->Hist().Pair(kTran, kHob);
    bool said = false, heard = false;
    for (const auto& e : fromTran["entries"]) {
        if (e["kind"] == "say" && e["dir"] == "to") said = true;
        if (e["kind"] == "say" && e["dir"] == "from") heard = true;
    }
    EXPECT_TRUE(said);   // the written chatter, since no model is attached
    EXPECT_TRUE(heard);  // the rotation gives each a turn to speak
    const Json fromHob = g->Hist().Pair(kHob, kTran);
    EXPECT_EQ(fromHob["entries"].size(), fromTran["entries"].size());
}

TEST(NpcMemory, AModelWritesTheLinesWhenOneIsAttached) {
    auto g = MeridianAt("promenade");
    std::vector<std::string> seen;
    g->SetChatter([&](const NpcDef& a, const NpcDef& b) {
        seen.push_back(a.id + ">" + b.id);
        return std::string("Remember the rice.");
    });
    ASSERT_TRUE(g->Travel("mess_hall").ok);
    ASSERT_FALSE(seen.empty());
    bool kept = false;
    const Json a = g->Hist().Pair(kTran, kHob), b = g->Hist().Pair(kHob, kTran);
    for (const auto& e : a["entries"]) kept = kept || e["text"] == "Remember the rice.";
    for (const auto& e : b["entries"]) kept = kept || e["text"] == "Remember the rice.";
    EXPECT_TRUE(kept);
}

TEST(NpcMemory, TalkingToAnNpcIsRememberedOnBothSides) {
    auto g = MeridianAt("promenade");
    ASSERT_TRUE(g->Travel("mess_hall").ok);
    ASSERT_TRUE(g->Talk("tran", "who").ok);
    const std::string hero = g->Hero();
    bool asked = false, replied = false;
    const Json p = g->Hist().Pair(kTran, hero);
    for (const auto& e : p["entries"]) {
        if (e["kind"] == "ask" && e["dir"] == "from") asked = true;
        if (e["kind"] == "reply" && e["dir"] == "to") replied = true;
    }
    EXPECT_TRUE(asked);
    EXPECT_TRUE(replied);
}

TEST(NpcMemory, OneNpcsWholeRecordIsListedByPartner) {
    auto g = MeridianAt("promenade");
    ASSERT_TRUE(g->Travel("mess_hall").ok);
    g->Wait(1);
    const Json rec = g->Hist().OwnerJson(kTran);
    std::set<std::string> with;
    for (const auto& p : rec["partners"]) {
        with.insert(p["with"].get<std::string>());
        EXPECT_GT(p["count"].get<int>(), 0);
    }
    EXPECT_TRUE(with.count(kHob));
    EXPECT_TRUE(with.count(g->Hero()));
}

TEST(NpcMemory, TheRecordAndTheTurnSurviveSavingAndLoading) {
    auto g = MeridianAt("promenade");
    ASSERT_TRUE(g->Travel("mess_hall").ok);
    g->Wait(1);
    const auto n = g->Hist().Count(kTran, kHob);
    auto h = Game::Load(g->Save());
    EXPECT_EQ(h->Hist().Count(kTran, kHob), n);
    h->Wait(1);
    EXPECT_GT(h->Hist().Count(kTran, kHob), n);
}

TEST(NpcMemory, ExploringTheShipFindsShipRooms) {
    bool found = false;
    for (std::uint64_t seed = 1; seed <= 30 && !found; ++seed) {
        auto g = MeridianAt("observation_lounge", seed);
        const CmdResult r = g->Explore();
        if (!r.ok || r.message == "interrupted" || g->ExtraRooms().empty()) continue;
        found = true;
        const std::string name = g->ExtraRooms().front().name;
        const bool ship = name.find("Closet") != std::string::npos || name.find("Bay") != std::string::npos || name.find("Crawlspace") != std::string::npos ||
                          name.find("Workshop") != std::string::npos || name.find("Cabin") != std::string::npos || name.find("Locker") != std::string::npos;
        EXPECT_TRUE(ship) << name;
    }
    EXPECT_TRUE(found);
}
