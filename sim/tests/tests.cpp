// Minimal dependency-free test runner: `dnd_tests` returns non-zero on any failure.
#include <iostream>
#include <sstream>
#include <fstream>
#include <filesystem>

#include "dnd/AI.hpp"
#include "dnd/Combat.hpp"
#include "dnd/Content.hpp"
#include "dnd/DM.hpp"
#include "dnd/Game.hpp"
#include "dnd/Human.hpp"
#include "dnd/Json.hpp"
#include "dnd/Llm.hpp"

using namespace dnd;

static int g_failures = 0;
#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) { ++g_failures; std::cerr << __FILE__ << ":" << __LINE__ << " CHECK failed: " #cond "\n"; } \
    } while (0)

static void TestDice() {
    auto e = DiceExpr::Parse("2d6+3");
    CHECK(e && e->count == 2 && e->sides == 6 && e->bonus == 3);
    CHECK(DiceExpr::Parse("1d8-1")->bonus == -1);
    CHECK(DiceExpr::Parse("5")->bonus == 5);
    CHECK(!DiceExpr::Parse("d"));
    CHECK(!DiceExpr::Parse("2d6+"));
    CHECK(DiceExpr::Parse("2d6+3")->ToString() == "2d6+3");

    DiceRoller::Seed(42);
    int lo = 99, hi = 0;
    for (int i = 0; i < 2000; ++i) { int r = DiceRoller::Roll(20); lo = std::min(lo, r); hi = std::max(hi, r); }
    CHECK(lo == 1 && hi == 20);
    for (int i = 0; i < 500; ++i) {
        CHECK(DiceRoller::D20(RollMode::Advantage).natural >= DiceRoller::D20(RollMode::Disadvantage).natural - 19);
        auto a = DiceRoller::D20(RollMode::Advantage);
        auto d = DiceRoller::D20(RollMode::Disadvantage);
        CHECK(a.natural >= a.discarded);
        CHECK(d.natural <= d.discarded);
    }
    CHECK(CombineModes(true, true) == RollMode::Normal);
    CHECK(CombineModes(true, false) == RollMode::Advantage);
    CHECK(AbilityModifier(8) == -1 && AbilityModifier(9) == -1 && AbilityModifier(10) == 0 && AbilityModifier(16) == 3);
}

static void TestGrid() {
    TacticalGrid g(10, 10);
    CHECK(g.HasLineOfSight({0, 0}, {9, 0}));
    g.Set({5, 0}, Terrain::Wall);
    CHECK(!g.HasLineOfSight({0, 0}, {9, 0}));
    CHECK(g.CoverBetween({0, 0}, {9, 0}) == Cover::Total);

    g.Set({5, 0}, Terrain::HalfCover);
    CHECK(g.CoverBetween({0, 0}, {9, 0}) == Cover::Half);
    g.Set({5, 0}, Terrain::ThreeQuarterCover);
    CHECK(g.CoverBetween({0, 0}, {9, 0}) == Cover::ThreeQuarters);
    CHECK(DistanceFt({0, 0}, {3, 7}) == 35);

    TacticalGrid p(6, 3);
    for (int y = 0; y < 2; ++y) p.Set({3, y}, Terrain::Wall);  // wall with a gap on y=2
    auto pm = p.ComputePaths({0, 0}, [](Pos) { return true; });
    CHECK(pm.CostTo({5, 0}) == 25);  // diagonal around the wall: 5 steps
    CHECK(pm.PathTo({5, 0}).size() == 5);
    p.Set({1, 0}, Terrain::Difficult);
    pm = p.ComputePaths({0, 0}, [](Pos) { return true; });
    CHECK(pm.CostTo({1, 0}) == 10);
    CHECK(pm.CostTo({3, 0}) == kUnreachable);
}

static void TestDm() {
    CHECK(DungeonMasterEngine::XpThreshold(3, Difficulty::Hard) == 225);
    CHECK(DungeonMasterEngine::PartyThreshold({3, 3, 3, 3}, Difficulty::Medium) == 600);
    CHECK(DungeonMasterEngine::Multiplier(1) == 1.0 && DungeonMasterEngine::Multiplier(4) == 2.0);

    DiceRoller::Seed(7);
    DungeonMasterEngine dm(std::make_unique<AIController>());
    const std::vector<int> party{3, 3, 3, 3};
    for (auto d : {Difficulty::Easy, Difficulty::Medium, Difficulty::Hard, Difficulty::Deadly}) {
        auto enc = dm.Generate(party, d);
        const int lo = DungeonMasterEngine::PartyThreshold(party, d);
        CHECK(!enc.monsters.empty());
        CHECK(enc.adjustedXp >= lo);
        if (d != Difficulty::Deadly) CHECK(enc.adjustedXp < DungeonMasterEngine::PartyThreshold(party, static_cast<Difficulty>(static_cast<int>(d) + 1)));
    }
}

static void TestRules() {
    EventBus bus;
    Combat c(TacticalGrid(10, 5), bus);
    CharacterSheet pcs;
    pcs.name = "Hero"; pcs.maxHp = pcs.hp = 10; pcs.armorClass = 10;
    CharacterSheet mon = pcs;
    mon.name = "Rat"; mon.maxHp = mon.hp = 5;
    auto& hero = c.AddActor(Team::Party, pcs, {1, 1}, nullptr);
    auto& rat = c.AddActor(Team::Monsters, mon, {2, 1}, nullptr);

    // Drop a PC to 0, then fail death saves with crits.
    CHECK(hero.ApplyDamage(10, false) == DamageResult::Dropped);
    CHECK(hero.life == LifeState::Unconscious);
    CHECK(hero.ApplyDamage(1, true) == DamageResult::DeathSaveFailed && hero.deathSaveFailures == 2);
    CHECK(hero.ApplyDamage(1, false) == DamageResult::Killed);

    hero.life = LifeState::Alive; hero.sheet.hp = 10;
    CHECK(hero.ApplyDamage(20, false) == DamageResult::Killed);  // massive damage
    hero.life = LifeState::Alive; hero.sheet.hp = 0; hero.life = LifeState::Unconscious;
    CHECK(hero.Heal(4) == 4 && hero.life == LifeState::Alive);

    CHECK(rat.ApplyDamage(5, false) == DamageResult::Killed);  // monsters just die

    // Ranged attack with an enemy adjacent is made at disadvantage; downed targets grant advantage + crit.
    GameActor& h2 = c.AddActor(Team::Party, pcs, {5, 3}, nullptr);
    GameActor& m2 = c.AddActor(Team::Monsters, mon, {6, 3}, nullptr);
    auto pv = c.PreviewAttack(h2, m2, true, 80, 320, false);
    CHECK(pv.legal && pv.mode == RollMode::Disadvantage);
    m2.life = LifeState::Unconscious;
    pv = c.PreviewAttack(h2, m2, false, 5, 0, false);
    CHECK(pv.legal && pv.mode == RollMode::Advantage && pv.autoCritOnHit);
}


static CharacterSheet Hero(const char* id, int level = 5) { return Content::BuildCharacter(*Content::FindCharacter(id), level); }
static CharacterSheet Mon(const char* id) { return Content::FindMonster(id)->sheet; }

static void TestContent() {
    CHECK(Content::IsLoaded());
    CHECK(!Content::Modules().empty() && Content::FindModule("ravenmoor"));
    CHECK(Content::FindSpell("fireball") && Content::FindMonster("goblin") && Content::FindClass("barbarian"));
    const auto thorin = Hero("thorin", 5);
    CHECK(thorin.attacksPerAction == 2);                       // Extra Attack at 5
    CHECK(Hero("thorin", 4).attacksPerAction == 1);
    CHECK(thorin.proficiency == 3 && thorin.maxHp > Hero("thorin", 3).maxHp);
    const auto elara = Hero("elara", 5);
    CHECK(elara.slotsMax[1] == 4 && elara.slotsMax[2] == 3 && elara.slotsMax[3] == 2);
    CHECK(thorin.FeatureUses("second_wind") == 1);
    CHECK(Hero("gruk", 3).FeatureUses("rage") == 3);
    CHECK(Content::XpForLevel(5) == 6500 && Content::LevelForXp(6499) == 4 && Content::LevelForXp(6500) == 5);
    // Every module location can be reached and every monster id resolves (Load validates, so just prove it throws on junk).
    bool threw = false;
    try { Content::Load("/definitely/not/a/dir"); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    CHECK(Content::IsLoaded());  // a failed Load must not unload the previous library
}

static void TestRng() {
    Rng a(123), b(999);
    for (int i = 0; i < 17; ++i) a.Uniform(1, 20);
    const std::string st = a.State();
    CHECK(b.SetState(st));
    for (int i = 0; i < 100; ++i) CHECK(a.Uniform(1, 6) == b.Uniform(1, 6));
    CHECK(!b.SetState("garbage"));
    // A scoped RNG makes DiceRoller deterministic and independent of the global one.
    Rng r1(5), r2(5);
    int x, y;
    { DiceRoller::Scope s(r1); x = DiceRoller::Roll(1000); }
    { DiceRoller::Scope s(r2); y = DiceRoller::Roll(1000); }
    CHECK(x == y);
}

static void TestJsonRoundTrip() {
    for (const auto& c : Content::Characters()) {
        const auto j = ToJson(Content::BuildCharacter(c));
        CHECK(ToJson(SheetFromJson(j)) == j);
    }
    for (const auto& m : Content::Monsters()) {
        const auto j = ToJson(m.sheet);
        CHECK(ToJson(SheetFromJson(j)) == j);
    }
    Action a;
    std::string err;
    CHECK(ActionFromJson(Json::parse(R"({"type":"move","dest":[3,4]})"), a, err) && a.type == ActionType::Move && a.dest.x == 3);
    CHECK(ActionFromJson(Json::parse(R"({"type":"cast","spell":"fireball","target":2})"), a, err) && a.spellId == "fireball");
    CHECK(!ActionFromJson(Json::parse(R"({"type":"move"})"), a, err));
    CHECK(!ActionFromJson(Json::parse(R"({"type":"await"})"), a, err));
    CHECK(!ActionFromJson(Json::parse(R"({"type":"bogus"})"), a, err));
}

static void TestAdjustDamage() {
    GameActor g;
    g.sheet = Mon("skeleton");
    CHECK(Combat::AdjustDamage(g, 10, DamageType::Bludgeoning, false) == 20);  // vulnerable
    CHECK(Combat::AdjustDamage(g, 10, DamageType::Poison, false) == 0);        // immune
    GameActor w;
    w.sheet = Mon("vampire_spawn");
    CHECK(Combat::AdjustDamage(w, 10, DamageType::Slashing, false) == 5);      // nonmagical physical resisted
    CHECK(Combat::AdjustDamage(w, 10, DamageType::Slashing, true) == 10);      // magic weapon bypasses it
}

static void TestExtraAttackAndConcentration() {
    EventBus bus;
    Combat c(TacticalGrid(12, 5), bus);
    auto& thorin = c.AddActor(Team::Party, Hero("thorin"), {1, 1}, nullptr);
    auto& aldric = c.AddActor(Team::Party, Hero("aldric"), {2, 2}, nullptr);
    CharacterSheet dummy = Mon("ogre");
    dummy.maxHp = dummy.hp = 500;
    auto& ogre = c.AddActor(Team::Monsters, dummy, {2, 1}, nullptr);

    thorin.sheet.economy.Reset(thorin.SpeedFt());
    Action atk;
    atk.type = ActionType::Attack;
    atk.targetId = ogre.id;
    CHECK(c.Resolve(thorin, atk).ok);
    CHECK(!thorin.sheet.economy.action && thorin.sheet.economy.attacksLeft == 1);  // the second swing is still owed
    CHECK(c.Resolve(thorin, atk).ok);
    CHECK(thorin.sheet.economy.attacksLeft == 0);
    CHECK(!c.Resolve(thorin, atk).ok);                                             // no third attack

    // Shield of Faith is a concentration buff; a second concentration spell replaces it.
    aldric.sheet.economy.Reset(aldric.SpeedFt());
    Action sof;
    sof.type = ActionType::CastSpell;
    sof.spellId = "shield_of_faith";
    sof.targetId = thorin.id;
    const int baseAc = thorin.sheet.armorClass;
    CHECK(c.Resolve(aldric, sof).ok);
    CHECK(thorin.EffectiveAc() == baseAc + 2 && aldric.concentrating == "shield_of_faith");
    CharacterSheet human = Mon("cultist");
    auto& cultist = c.AddActor(Team::Monsters, human, {3, 2}, nullptr);
    Action hold;
    hold.type = ActionType::CastSpell;
    hold.spellId = "hold_person";
    hold.targetId = cultist.id;
    CHECK(c.Resolve(aldric, hold).ok);
    CHECK(aldric.concentrating == "hold_person");
    CHECK(thorin.EffectiveAc() == baseAc);  // the old spell ended when concentration moved
    // Hold Person only works on humanoids.
    aldric.sheet.economy.Reset(aldric.SpeedFt());
    Action holdOgre = hold;
    holdOgre.targetId = ogre.id;
    CHECK(!c.Resolve(aldric, holdOgre).ok);
}

static void TestRemoteCombat() {
    // A RemoteController pauses the engine; Submit resumes it. Saving while paused and loading elsewhere continues identically.
    auto run = [](bool saveAndReload) {
        GameConfig cfg;
        cfg.rooms = 1;
        cfg.difficulty = "easy";
        cfg.seed = 11;
        cfg.role = Role::Player;
        cfg.hero = "elara";
        auto g = std::make_unique<Game>(cfg);
        CHECK(g->GetMode() == Mode::Combat);
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
        CHECK(prompts >= 1);
        CHECK(g->GetMode() != Mode::Combat);
        return g->Save().dump();
    };
    CHECK(run(false) == run(true));
}

static void TestGameModule() {
    GameConfig cfg;
    cfg.moduleId = "ravenmoor";
    cfg.seed = 21;
    cfg.role = Role::Spectator;
    Game g(cfg);
    CHECK(g.GetMode() == Mode::Exploring && g.LocationId() == Content::FindModule("ravenmoor")->start);
    CHECK(g.HasFlag("relic") && (g.FlagValue("relic") == "old_chapel" || g.FlagValue("relic") == "standing_stones"));
    CHECK(!g.Travel("nowhere").ok);

    CHECK(g.Travel("greywick").ok || g.GetMode() == Mode::Combat);
    while (g.GetMode() == Mode::Combat) g.AutoStep();
    CHECK(g.LocationId() == "greywick");
    CHECK(!g.Travel("crypt").ok);                      // blocked until the silver key is found
    CHECK(g.Rest("long").ok || g.GetMode() == Mode::Combat);
    while (g.GetMode() == Mode::Combat) g.AutoStep();
    const auto again = g.Rest("long");
    CHECK(!again.ok);                                   // SRD: one long rest per 24 hours
    CHECK(!g.Talk("marta", "no_such_topic").ok);
    CHECK(g.Talk("marta", "rumours").ok && g.HasFlag("heard_of_key"));

    // Autopilot clears the whole module on most seeds and always terminates.
    int done = 0;
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
        GameConfig c = cfg;
        c.seed = seed;
        Game x(c);
        int steps = 0;
        while ((x.GetMode() == Mode::Exploring || x.GetMode() == Mode::Combat) && steps++ < 4000)
            if (!x.AutoStep().ok) break;
        CHECK(x.GetMode() == Mode::Complete || x.GetMode() == Mode::Defeated);
        done += x.GetMode() == Mode::Complete;
        if (x.GetMode() == Mode::Complete) CHECK(x.HasFlag("countess_defeated") && x.HasFlag("has_silver_key"));
    }
    CHECK(done >= 3);
}

static void TestSaveLoadDeterminism() {
    GameConfig cfg;
    cfg.moduleId = "ravenmoor";
    cfg.seed = 5;
    cfg.role = Role::Spectator;
    Game a(cfg);
    for (int i = 0; i < 18; ++i) a.AutoStep();
    auto b = Game::Load(a.Save());
    CHECK(a.Save() == b->Save());
    for (int i = 0; i < 300 && a.GetMode() != Mode::Complete && a.GetMode() != Mode::Defeated; ++i) {
        a.AutoStep();
        b->AutoStep();
    }
    CHECK(a.Save().dump() == b->Save().dump());
    // Same seed, same story.
    Game c(cfg);
    for (int i = 0; i < 18; ++i) c.AutoStep();
    Game d(cfg);
    for (int i = 0; i < 18; ++i) d.AutoStep();
    CHECK(c.Save().dump() == d.Save().dump());
}

static void TestProceduralDelve() {
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
        CHECK(g.GetMode() == Mode::Complete || g.GetMode() == Mode::Defeated);
    }
}

static void TestRoleSwap() {
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
    CHECK(g.GetMode() != Mode::Combat);
    CHECK(log.find("[role is now: dungeon master]") != std::string::npos);
    CHECK(log.find("[role is now: spectator]") != std::string::npos);
}

static void TestLlm() {
    CHECK(StripThink("<think>plotting</think>  Hello") == "Hello");
    CHECK(StripThink("reasoning leaked</think>Answer") == "Answer");
    CHECK(ExtractJsonObject("sure! ```json\n{\"a\":{\"b\":\"}\"}}\n``` done") == "{\"a\":{\"b\":\"}\"}}");
    std::string err;
    auto plan = LlmDmController::ParsePlan("```json\n{\"actions\":[{\"type\":\"dash\"},{\"type\":\"end_turn\"}]}\n```", err);
    CHECK(plan.size() == 2 && plan[0].type == ActionType::Dash);
    CHECK(LlmDmController::ParsePlan("I attack!", err).empty() && !err.empty());
    CHECK(LlmDmController::ParsePlan(R"({"actions":[{"type":"explode"}]})", err).empty());

    auto runWith = [](std::shared_ptr<MockLlmClient> mock, bool dm, bool narration) {
        GameConfig cfg;
        cfg.rooms = 1; cfg.difficulty = "easy"; cfg.seed = 3; cfg.role = Role::Spectator;
        cfg.llmDm = dm; cfg.llmNarration = narration;
        Game g(cfg);
        if (narration) g.SetNarrator(MakeLlmNarrator(mock, g));
        if (dm) g.SetMonsterFactory([mock] { return std::make_unique<LlmDmController>(mock); });
        int guard = 0;
        while (g.GetMode() == Mode::Combat && guard++ < 500) g.AdvanceCombat();
        CHECK(g.GetMode() != Mode::Combat);  // the model can never stall a fight
        return g.Save();
    };
    // Garbage replies fall back to the built-in AI, so the result equals a plain AI run.
    auto junk = std::make_shared<MockLlmClient>([](const std::string&, const std::string&, bool) { LlmReply r; r.ok = true; r.content = "no idea, sorry"; return r; });
    auto failing = std::make_shared<MockLlmClient>([](const std::string&, const std::string&, bool) { LlmReply r; r.error = "boom"; return r; });
    const auto plain = runWith(std::make_shared<MockLlmClient>(), false, false);
    const auto viaJunk = runWith(junk, true, false);
    CHECK(viaJunk["rng"] == plain["rng"] && viaJunk["party"] == plain["party"]);
    CHECK(junk->Calls() > 0);
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
    CHECK(flavoured);
    // The model store resolves paths even when a model is absent.
    const auto mi = LocateModel("no-such-model");
    CHECK(!mi.exists && mi.path.find("no-such-model") != std::string::npos);
}

static void TestSay() {
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
    CHECK(chat.ok && chat.steps.empty() && chat.message == "A cold wind answers." && g.LocationId() == before);
    SayResult go = Say(mock, g, "leave for the village");
    CHECK(g.LocationId() == "greywick");      // the first step ran
    CHECK(!go.ok && go.steps.size() == 1);    // the invented exit was refused by the engine
    CHECK(Say(mock, g, "   ").ok == false);
    MockLlmClient broken([](const std::string&, const std::string&, bool) { LlmReply r; r.ok = false; r.error = "down"; return r; });
    CHECK(!Say(broken, g, "hello").ok);
}

int main() {
    try {
        Content::Load(DND_DATA_DIR);
    } catch (const std::exception& e) {
        std::cerr << "cannot load content from '" << DND_DATA_DIR << "': " << e.what() << "\n";
        return 2;
    }
    struct T { const char* name; void (*fn)(); };
    const T tests[] = {{"Dice", TestDice}, {"Grid", TestGrid}, {"Dm", TestDm}, {"Rules", TestRules}, {"Content", TestContent},
                       {"Rng", TestRng}, {"JsonRoundTrip", TestJsonRoundTrip}, {"AdjustDamage", TestAdjustDamage},
                       {"ExtraAttackAndConcentration", TestExtraAttackAndConcentration}, {"RemoteCombat", TestRemoteCombat},
                       {"GameModule", TestGameModule}, {"SaveLoadDeterminism", TestSaveLoadDeterminism},
                       {"ProceduralDelve", TestProceduralDelve}, {"RoleSwap", TestRoleSwap}, {"Llm", TestLlm}, {"Say", TestSay}};
    for (const auto& t : tests) {
        try {
            t.fn();
        } catch (const std::exception& e) {
            ++g_failures;
            std::cerr << "test " << t.name << " threw: " << e.what() << "\n";
        } catch (...) {
            ++g_failures;
            std::cerr << "test " << t.name << " threw an unknown exception\n";
        }
    }
    if (g_failures) { std::cerr << g_failures << " check(s) failed\n"; return 1; }
    std::cout << "all tests passed\n";
    return 0;
}
