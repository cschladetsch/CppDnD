// Minimal dependency-free test runner: `dnd_tests` returns non-zero on any failure.
#include <iostream>
#include <sstream>

#include "dnd/AI.hpp"
#include "dnd/Combat.hpp"
#include "dnd/Content.hpp"
#include "dnd/DM.hpp"
#include "dnd/Session.hpp"

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

static void TestFullFights() {
    for (std::uint64_t seed = 1; seed <= 30; ++seed) {
        std::istringstream in;
        std::ostringstream out1, out2;
        Session a(in, out1, seed);
        a.SetRole(Role::Spectator);
        const auto r1 = a.RunEncounter(static_cast<Difficulty>(seed % 4));
        Session b(in, out2, seed);
        b.SetRole(Role::Spectator);
        const auto r2 = b.RunEncounter(static_cast<Difficulty>(seed % 4));
        CHECK(r1.rounds > 0 && r1.rounds <= 30);
        CHECK(r1.outcome == r2.outcome && r1.rounds == r2.rounds);  // seeded => reproducible
        CHECK(out1.str() == out2.str());
    }
}

static void TestRoleSwap() {
    // A scripted human playing Thorin swaps to DM on the first prompt, then input ends,
    // which hands everything to the AI. The fight must still finish.
    std::istringstream in("swap dm\n");
    std::ostringstream out;
    Session s(in, out, 99);
    s.SetRole(Role::Player);
    const auto r = s.RunEncounter(Difficulty::Easy);
    CHECK(r.rounds > 0);
    CHECK(out.str().find("[role is now: dungeon master]") != std::string::npos);
    CHECK(out.str().find("[role is now: spectator]") != std::string::npos);
}

int main() {
    TestDice(); TestGrid(); TestDm(); TestRules(); TestFullFights(); TestRoleSwap();
    if (g_failures) { std::cerr << g_failures << " check(s) failed\n"; return 1; }
    std::cout << "all tests passed\n";
    return 0;
}
