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


TEST(Core, Dice) {
    auto e = DiceExpr::Parse("2d6+3");
    EXPECT_TRUE(e && e->count == 2 && e->sides == 6 && e->bonus == 3);
    EXPECT_EQ(DiceExpr::Parse("1d8-1")->bonus, -1);
    EXPECT_EQ(DiceExpr::Parse("5")->bonus, 5);
    EXPECT_FALSE(DiceExpr::Parse("d"));
    EXPECT_FALSE(DiceExpr::Parse("2d6+"));
    EXPECT_EQ(DiceExpr::Parse("2d6+3")->ToString(), "2d6+3");

    DiceRoller::Seed(42);
    int lo = 99, hi = 0;
    for (int i = 0; i < 2000; ++i) { int r = DiceRoller::Roll(20); lo = std::min(lo, r); hi = std::max(hi, r); }
    EXPECT_TRUE(lo == 1 && hi == 20);
    for (int i = 0; i < 500; ++i) {
        EXPECT_TRUE(DiceRoller::D20(RollMode::Advantage).natural >= DiceRoller::D20(RollMode::Disadvantage).natural - 19);
        auto a = DiceRoller::D20(RollMode::Advantage);
        auto d = DiceRoller::D20(RollMode::Disadvantage);
        EXPECT_TRUE(a.natural >= a.discarded);
        EXPECT_TRUE(d.natural <= d.discarded);
    }
    EXPECT_EQ(CombineModes(true, true), RollMode::Normal);
    EXPECT_EQ(CombineModes(true, false), RollMode::Advantage);
    EXPECT_TRUE(AbilityModifier(8) == -1 && AbilityModifier(9) == -1 && AbilityModifier(10) == 0 && AbilityModifier(16) == 3);
}

TEST(Core, Grid) {
    TacticalGrid g(10, 10);
    EXPECT_TRUE(g.HasLineOfSight({0, 0}, {9, 0}));
    g.Set({5, 0}, Terrain::Wall);
    EXPECT_FALSE(g.HasLineOfSight({0, 0}, {9, 0}));
    EXPECT_EQ(g.CoverBetween({0, 0}, {9, 0}), Cover::Total);

    g.Set({5, 0}, Terrain::HalfCover);
    EXPECT_EQ(g.CoverBetween({0, 0}, {9, 0}), Cover::Half);
    g.Set({5, 0}, Terrain::ThreeQuarterCover);
    EXPECT_EQ(g.CoverBetween({0, 0}, {9, 0}), Cover::ThreeQuarters);
    EXPECT_EQ(DistanceFt({0, 0}, {3, 7}), 35);

    TacticalGrid p(6, 3);
    for (int y = 0; y < 2; ++y) p.Set({3, y}, Terrain::Wall);  // wall with a gap on y=2
    auto pm = p.ComputePaths({0, 0}, [](Pos) { return true; });
    EXPECT_EQ(pm.CostTo({5, 0}), 25);  // diagonal around the wall: 5 steps
    EXPECT_EQ(pm.PathTo({5, 0}).size(), 5);
    p.Set({1, 0}, Terrain::Difficult);
    pm = p.ComputePaths({0, 0}, [](Pos) { return true; });
    EXPECT_EQ(pm.CostTo({1, 0}), 10);
    EXPECT_EQ(pm.CostTo({3, 0}), kUnreachable);
}

TEST(Core, Dm) {
    EXPECT_EQ(DungeonMasterEngine::XpThreshold(3, Difficulty::Hard), 225);
    EXPECT_EQ(DungeonMasterEngine::PartyThreshold({3, 3, 3, 3}, Difficulty::Medium), 600);
    EXPECT_TRUE(DungeonMasterEngine::Multiplier(1) == 1.0 && DungeonMasterEngine::Multiplier(4) == 2.0);

    DiceRoller::Seed(7);
    DungeonMasterEngine dm(std::make_unique<AIController>());
    const std::vector<int> party{3, 3, 3, 3};
    for (auto d : {Difficulty::Easy, Difficulty::Medium, Difficulty::Hard, Difficulty::Deadly}) {
        auto enc = dm.Generate(party, d);
        const int lo = DungeonMasterEngine::PartyThreshold(party, d);
        EXPECT_FALSE(enc.monsters.empty());
        EXPECT_TRUE(enc.adjustedXp >= lo);
        if (d != Difficulty::Deadly) EXPECT_TRUE(enc.adjustedXp < DungeonMasterEngine::PartyThreshold(party, static_cast<Difficulty>(static_cast<int>(d) + 1)));
    }
}

TEST(Core, Rules) {
    EventBus bus;
    Combat c(TacticalGrid(10, 5), bus);
    CharacterSheet pcs;
    pcs.name = "Hero"; pcs.maxHp = pcs.hp = 10; pcs.armorClass = 10;
    CharacterSheet mon = pcs;
    mon.name = "Rat"; mon.maxHp = mon.hp = 5;
    auto& hero = c.AddActor(Team::Party, pcs, {1, 1}, nullptr);
    auto& rat = c.AddActor(Team::Monsters, mon, {2, 1}, nullptr);

    // Drop a PC to 0, then fail death saves with crits.
    EXPECT_EQ(hero.ApplyDamage(10, false), DamageResult::Dropped);
    EXPECT_EQ(hero.life, LifeState::Unconscious);
    EXPECT_TRUE(hero.ApplyDamage(1, true) == DamageResult::DeathSaveFailed && hero.deathSaveFailures == 2);
    EXPECT_EQ(hero.ApplyDamage(1, false), DamageResult::Killed);

    hero.life = LifeState::Alive; hero.sheet.hp = 10;
    EXPECT_EQ(hero.ApplyDamage(20, false), DamageResult::Killed);  // massive damage
    hero.life = LifeState::Alive; hero.sheet.hp = 0; hero.life = LifeState::Unconscious;
    EXPECT_TRUE(hero.Heal(4) == 4 && hero.life == LifeState::Alive);

    EXPECT_EQ(rat.ApplyDamage(5, false), DamageResult::Killed);  // monsters just die

    // Ranged attack with an enemy adjacent is made at disadvantage; downed targets grant advantage + crit.
    GameActor& h2 = c.AddActor(Team::Party, pcs, {5, 3}, nullptr);
    GameActor& m2 = c.AddActor(Team::Monsters, mon, {6, 3}, nullptr);
    auto pv = c.PreviewAttack(h2, m2, true, 80, 320, false);
    EXPECT_TRUE(pv.legal && pv.mode == RollMode::Disadvantage);
    m2.life = LifeState::Unconscious;
    pv = c.PreviewAttack(h2, m2, false, 5, 0, false);
    EXPECT_TRUE(pv.legal && pv.mode == RollMode::Advantage && pv.autoCritOnHit);
}

TEST(Core, Content) {
    EXPECT_TRUE(Content::IsLoaded());
    EXPECT_TRUE(!Content::Modules().empty() && Content::FindModule("ravenmoor"));
    EXPECT_TRUE(Content::FindSpell("fireball") && Content::FindMonster("goblin") && Content::FindClass("barbarian"));
    const auto thorin = Hero("thorin", 5);
    EXPECT_EQ(thorin.attacksPerAction, 2);                       // Extra Attack at 5
    EXPECT_EQ(Hero("thorin", 4).attacksPerAction, 1);
    EXPECT_TRUE(thorin.proficiency == 3 && thorin.maxHp > Hero("thorin", 3).maxHp);
    const auto elara = Hero("elara", 5);
    EXPECT_TRUE(elara.slotsMax[1] == 4 && elara.slotsMax[2] == 3 && elara.slotsMax[3] == 2);
    EXPECT_EQ(thorin.FeatureUses("second_wind"), 1);
    EXPECT_EQ(Hero("gruk", 3).FeatureUses("rage"), 3);
    EXPECT_TRUE(Content::XpForLevel(5) == 6500 && Content::LevelForXp(6499) == 4 && Content::LevelForXp(6500) == 5);
    // Every module location can be reached and every monster id resolves (Load validates, so just prove it throws on junk).
    bool threw = false;
    try { Content::Load("/definitely/not/a/dir"); } catch (const std::exception&) { threw = true; }
    EXPECT_TRUE(threw);
    EXPECT_TRUE(Content::IsLoaded());  // a failed Load must not unload the previous library
}

TEST(Core, Rng) {
    Rng a(123), b(999);
    for (int i = 0; i < 17; ++i) a.Uniform(1, 20);
    const std::string st = a.State();
    EXPECT_TRUE(b.SetState(st));
    for (int i = 0; i < 100; ++i) EXPECT_EQ(a.Uniform(1, 6), b.Uniform(1, 6));
    EXPECT_FALSE(b.SetState("garbage"));
    // A scoped RNG makes DiceRoller deterministic and independent of the global one.
    Rng r1(5), r2(5);
    int x, y;
    { DiceRoller::Scope s(r1); x = DiceRoller::Roll(1000); }
    { DiceRoller::Scope s(r2); y = DiceRoller::Roll(1000); }
    EXPECT_EQ(x, y);
}

TEST(Core, JsonRoundTrip) {
    for (const auto& c : Content::Characters()) {
        const auto j = ToJson(Content::BuildCharacter(c));
        EXPECT_EQ(ToJson(SheetFromJson(j)), j);
    }
    for (const auto& m : Content::Monsters()) {
        const auto j = ToJson(m.sheet);
        EXPECT_EQ(ToJson(SheetFromJson(j)), j);
    }
    Action a;
    std::string err;
    EXPECT_TRUE(ActionFromJson(Json::parse(R"({"type":"move","dest":[3,4]})"), a, err) && a.type == ActionType::Move && a.dest.x == 3);
    EXPECT_TRUE(ActionFromJson(Json::parse(R"({"type":"cast","spell":"fireball","target":2})"), a, err) && a.spellId == "fireball");
    EXPECT_FALSE(ActionFromJson(Json::parse(R"({"type":"move"})"), a, err));
    EXPECT_FALSE(ActionFromJson(Json::parse(R"({"type":"await"})"), a, err));
    EXPECT_FALSE(ActionFromJson(Json::parse(R"({"type":"bogus"})"), a, err));
}

TEST(Core, AdjustDamage) {
    GameActor g;
    g.sheet = Mon("skeleton");
    EXPECT_EQ(Combat::AdjustDamage(g, 10, DamageType::Bludgeoning, false), 20);  // vulnerable
    EXPECT_EQ(Combat::AdjustDamage(g, 10, DamageType::Poison, false), 0);        // immune
    GameActor w;
    w.sheet = Mon("vampire_spawn");
    EXPECT_EQ(Combat::AdjustDamage(w, 10, DamageType::Slashing, false), 5);      // nonmagical physical resisted
    EXPECT_EQ(Combat::AdjustDamage(w, 10, DamageType::Slashing, true), 10);      // magic weapon bypasses it
}

TEST(Core, ExtraAttackAndConcentration) {
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
    EXPECT_TRUE(c.Resolve(thorin, atk).ok);
    EXPECT_TRUE(!thorin.sheet.economy.action && thorin.sheet.economy.attacksLeft == 1);  // the second swing is still owed
    EXPECT_TRUE(c.Resolve(thorin, atk).ok);
    EXPECT_EQ(thorin.sheet.economy.attacksLeft, 0);
    EXPECT_FALSE(c.Resolve(thorin, atk).ok);                                             // no third attack

    // Shield of Faith is a concentration buff; a second concentration spell replaces it.
    aldric.sheet.economy.Reset(aldric.SpeedFt());
    Action sof;
    sof.type = ActionType::CastSpell;
    sof.spellId = "shield_of_faith";
    sof.targetId = thorin.id;
    const int baseAc = thorin.sheet.armorClass;
    EXPECT_TRUE(c.Resolve(aldric, sof).ok);
    EXPECT_TRUE(thorin.EffectiveAc() == baseAc + 2 && aldric.concentrating == "shield_of_faith");
    CharacterSheet human = Mon("cultist");
    auto& cultist = c.AddActor(Team::Monsters, human, {3, 2}, nullptr);
    Action hold;
    hold.type = ActionType::CastSpell;
    hold.spellId = "hold_person";
    hold.targetId = cultist.id;
    EXPECT_TRUE(c.Resolve(aldric, hold).ok);
    EXPECT_EQ(aldric.concentrating, "hold_person");
    EXPECT_EQ(thorin.EffectiveAc(), baseAc);  // the old spell ended when concentration moved
    // Hold Person only works on humanoids.
    aldric.sheet.economy.Reset(aldric.SpeedFt());
    Action holdOgre = hold;
    holdOgre.targetId = ogre.id;
    EXPECT_FALSE(c.Resolve(aldric, holdOgre).ok);
}
