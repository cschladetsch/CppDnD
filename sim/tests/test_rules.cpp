// Rules for helping and hurting party members, potions, spell targeting, movement,
// and the animation fields the engine puts on its events.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>

#include "dnd/Combat.hpp"
#include "dnd/Content.hpp"
#include "dnd/Json.hpp"
#include "test_support.hpp"

using namespace dnd;

namespace {

// An open 12x6 field with a recorded event log. Actors have no controllers: the tests call Resolve directly.
struct Field {
    EventBus bus;
    std::vector<Event> seen;
    Combat c;
    explicit Field(TacticalGrid g = TacticalGrid(12, 6)) : c(std::move(g), bus) {
        bus.Subscribe([this](const Event& e) { seen.push_back(e); });
    }
    GameActor& Add(Team team, CharacterSheet s, Pos p) {
        auto& a = c.AddActor(team, std::move(s), p, nullptr);
        a.sheet.economy.Reset(a.SpeedFt());
        return a;
    }
    const Event* First(EventType t) const {
        const auto it = std::find_if(seen.begin(), seen.end(), [t](const Event& e) { return e.type == t; });
        return it == seen.end() ? nullptr : &*it;
    }
};

Action Cast(const char* spell, int target, int slot = 0) {
    Action a;
    a.type = ActionType::CastSpell;
    a.spellId = spell;
    a.targetId = target;
    a.slotLevel = slot;
    return a;
}
Action Potion(int target) {
    Action a;
    a.type = ActionType::UsePotion;
    a.targetId = target;
    return a;
}
Action Move(int x, int y) {
    Action a;
    a.type = ActionType::Move;
    a.dest = {x, y};
    return a;
}
bool Says(const ActionResult& r, const char* text) { return !r.ok && r.reason.find(text) != std::string::npos; }

}  // namespace

// ---- party members are never attack targets, but can be helped

TEST(Rules, AttackingAPartyMemberIsRefused) {
    Field f;
    auto& thorin = f.Add(Team::Party, Hero("thorin"), {1, 1});
    auto& aldric = f.Add(Team::Party, Hero("aldric"), {2, 1});
    Action atk;
    atk.type = ActionType::Attack;
    atk.targetId = aldric.id;
    EXPECT_FALSE(f.c.Resolve(thorin, atk).ok);
    EXPECT_EQ(aldric.sheet.hp, aldric.sheet.maxHp);
}

TEST(Rules, HarmfulSingleTargetSpellOnAnAllyIsRefused) {
    Field f;
    auto& elara = f.Add(Team::Party, Hero("elara"), {1, 1});
    auto& thorin = f.Add(Team::Party, Hero("thorin"), {4, 1});
    EXPECT_TRUE(Says(f.c.Resolve(elara, Cast("fire_bolt", thorin.id)), "cannot target an ally"));
}

TEST(Rules, HealingSpellOnAnEnemyIsRefused) {
    Field f;
    auto& aldric = f.Add(Team::Party, Hero("aldric"), {1, 1});
    auto& goblin = f.Add(Team::Monsters, Mon("goblin"), {2, 1});
    EXPECT_TRUE(Says(f.c.Resolve(aldric, Cast("cure_wounds", goblin.id)), "only target allies"));
}

TEST(Rules, PotionHealsAnAdjacentAllyAndUsesTheAction) {
    Field f;
    auto& finn = f.Add(Team::Party, Hero("finn"), {1, 1});
    auto& thorin = f.Add(Team::Party, Hero("thorin"), {2, 2});
    thorin.sheet.hp = 3;
    const int before = finn.sheet.potions;
    ASSERT_TRUE(f.c.Resolve(finn, Potion(thorin.id)).ok);
    EXPECT_GT(thorin.sheet.hp, 3);
    EXPECT_EQ(finn.sheet.potions, before - 1);
    EXPECT_FALSE(finn.sheet.economy.action);
    ASSERT_NE(f.First(EventType::Heal), nullptr);
    EXPECT_EQ(f.First(EventType::Heal)->target, thorin.id);
}

TEST(Rules, PotionForAnAllyTenFeetAwayIsRefused) {
    Field f;
    auto& finn = f.Add(Team::Party, Hero("finn"), {1, 1});
    auto& thorin = f.Add(Team::Party, Hero("thorin"), {3, 1});
    EXPECT_TRUE(Says(f.c.Resolve(finn, Potion(thorin.id)), "too far"));
    EXPECT_EQ(finn.sheet.potions, 1);
}

TEST(Rules, PotionWithNoneLeftIsRefused) {
    Field f;
    auto& elara = f.Add(Team::Party, Hero("elara"), {1, 1});
    auto& thorin = f.Add(Team::Party, Hero("thorin"), {2, 1});
    elara.sheet.potions = 0;
    EXPECT_TRUE(Says(f.c.Resolve(elara, Potion(thorin.id)), "no potions"));
}

TEST(Rules, PotionForAnEnemyIsRefused) {
    Field f;
    auto& finn = f.Add(Team::Party, Hero("finn"), {1, 1});
    auto& goblin = f.Add(Team::Monsters, Mon("goblin"), {2, 1});
    EXPECT_TRUE(Says(f.c.Resolve(finn, Potion(goblin.id)), "invalid potion target"));
}

TEST(Rules, HealingWordTakesTheBonusActionAndASlot) {
    Field f;
    auto& aldric = f.Add(Team::Party, Hero("aldric"), {1, 1});
    auto& thorin = f.Add(Team::Party, Hero("thorin"), {7, 1});   // 30 ft: in range of Healing Word, not of Cure Wounds
    thorin.sheet.hp = 2;
    const int slots = aldric.sheet.slots[1];
    ASSERT_TRUE(f.c.Resolve(aldric, Cast("healing_word", thorin.id, 1)).ok);
    EXPECT_GT(thorin.sheet.hp, 2);
    EXPECT_TRUE(aldric.sheet.economy.action);
    EXPECT_FALSE(aldric.sheet.economy.bonusAction);
    EXPECT_EQ(aldric.sheet.slots[1], slots - 1);
}

TEST(Rules, TouchSpellOutOfRangeIsRefused) {
    Field f;
    auto& aldric = f.Add(Team::Party, Hero("aldric"), {1, 1});
    auto& thorin = f.Add(Team::Party, Hero("thorin"), {5, 1});
    EXPECT_TRUE(Says(f.c.Resolve(aldric, Cast("cure_wounds", thorin.id, 1)), "out of range"));
}

TEST(Rules, LevelledSpellWithNoSlotsLeftIsRefused) {
    Field f;
    auto& elara = f.Add(Team::Party, Hero("elara"), {1, 1});
    auto& goblin = f.Add(Team::Monsters, Mon("goblin"), {5, 1});
    elara.sheet.slots.fill(0);
    EXPECT_FALSE(f.c.Resolve(elara, Cast("magic_missile", goblin.id)).ok);
    EXPECT_TRUE(f.c.Resolve(elara, Cast("fire_bolt", goblin.id)).ok);   // a cantrip needs no slot
}

// ---- what a cast tells the client so it can animate it

TEST(Rules, MagicMissileNamesItsSpellAndElement) {
    Field f;
    auto& elara = f.Add(Team::Party, Hero("elara"), {1, 1});
    auto& goblin = f.Add(Team::Monsters, Mon("goblin"), {6, 1});
    ASSERT_TRUE(f.c.Resolve(elara, Cast("magic_missile", goblin.id, 1)).ok);
    const Event* e = f.First(EventType::Spell);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->spell, "magic_missile");
    EXPECT_EQ(e->element, "force");
    EXPECT_EQ(e->radiusFt, 0);
}

TEST(Rules, OnlyTheFirstEventOfABurstIsStamped) {
    Field f;
    auto& elara = f.Add(Team::Party, Hero("elara"), {1, 1});
    auto& g1 = f.Add(Team::Monsters, Mon("goblin"), {8, 3});
    f.Add(Team::Monsters, Mon("goblin"), {9, 3});
    ASSERT_TRUE(f.c.Resolve(elara, Cast("shatter", g1.id, 2)).ok);
    std::vector<const Event*> spells;
    for (const auto& e : f.seen)
        if (e.type == EventType::Spell) spells.push_back(&e);
    ASSERT_GE(spells.size(), 3u);   // the burst, then a save for each goblin
    EXPECT_EQ(spells[0]->spell, "shatter");
    EXPECT_EQ(spells[0]->element, "thunder");
    EXPECT_EQ(spells[0]->radiusFt, 10);
    for (std::size_t i = 1; i < spells.size(); ++i) EXPECT_TRUE(spells[i]->spell.empty());
}

TEST(Rules, HealingSpellEventIsMarkedHealing) {
    Field f;
    auto& aldric = f.Add(Team::Party, Hero("aldric"), {1, 1});
    auto& thorin = f.Add(Team::Party, Hero("thorin"), {2, 1});
    thorin.sheet.hp = 1;
    ASSERT_TRUE(f.c.Resolve(aldric, Cast("cure_wounds", thorin.id, 1)).ok);
    const Event* e = f.First(EventType::Heal);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->spell, "cure_wounds");
    EXPECT_EQ(e->element, "healing");
}

TEST(Rules, BuffRaisesArmourAndIsMarkedBuff) {
    Field f;
    auto& aldric = f.Add(Team::Party, Hero("aldric"), {1, 1});
    auto& thorin = f.Add(Team::Party, Hero("thorin"), {3, 1});
    const int ac = thorin.EffectiveAc();
    ASSERT_TRUE(f.c.Resolve(aldric, Cast("shield_of_faith", thorin.id, 1)).ok);
    EXPECT_EQ(thorin.EffectiveAc(), ac + 2);
    ASSERT_NE(f.First(EventType::Spell), nullptr);
    EXPECT_EQ(f.First(EventType::Spell)->element, "buff");
}

TEST(Rules, PotionHealIsNotMarkedAsASpell) {
    Field f;
    auto& finn = f.Add(Team::Party, Hero("finn"), {1, 1});
    finn.sheet.hp = 2;
    ASSERT_TRUE(f.c.Resolve(finn, Potion(finn.id)).ok);
    ASSERT_NE(f.First(EventType::Heal), nullptr);
    EXPECT_TRUE(f.First(EventType::Heal)->spell.empty());
}

// ---- movement

TEST(Rules, MovingIntoAWallIsRefused) {
    TacticalGrid g(12, 6);
    g.Set({3, 1}, Terrain::Wall);
    Field f(std::move(g));
    auto& thorin = f.Add(Team::Party, Hero("thorin"), {1, 1});
    EXPECT_TRUE(Says(f.c.Resolve(thorin, Move(3, 1)), "blocked"));
    EXPECT_EQ(thorin.pos.x, 1);
}

TEST(Rules, MovingOffTheMapIsRefused) {
    Field f;
    auto& thorin = f.Add(Team::Party, Hero("thorin"), {1, 1});
    EXPECT_TRUE(Says(f.c.Resolve(thorin, Move(20, 1)), "off the map"));
}

TEST(Rules, MovePathIsOneSquareAtATimeAndPaysForItself) {
    TacticalGrid g(12, 6);
    for (int y = 0; y < 4; ++y) g.Set({4, y}, Terrain::Wall);   // a wall to walk around
    Field f(std::move(g));
    auto& elara = f.Add(Team::Party, Hero("elara"), {2, 1});
    const int budget = elara.sheet.economy.movementFt;
    ASSERT_TRUE(f.c.Resolve(elara, Move(6, 1)).ok);
    const Event* e = f.First(EventType::Move);
    ASSERT_NE(e, nullptr);
    const auto& p = e->path;
    ASSERT_GE(p.size(), 4u);
    EXPECT_EQ(p[0], 2);
    EXPECT_EQ(p[1], 1);
    for (std::size_t i = 2; i + 1 < p.size(); i += 2) {
        EXPECT_LE(std::abs(p[i] - p[i - 2]), 1);
        EXPECT_LE(std::abs(p[i + 1] - p[i - 1]), 1);
        EXPECT_NE(p[i + 1] < 4 && p[i] == 4, true) << "walked through the wall";
    }
    EXPECT_EQ(p[p.size() - 2], elara.pos.x);
    EXPECT_EQ(p.back(), elara.pos.y);
    EXPECT_EQ(static_cast<int>(p.size() / 2 - 1) * 5, e->amount);   // open ground: 5 ft a square
    EXPECT_EQ(elara.sheet.economy.movementFt, budget - e->amount);
}

// ---- the event fields on the wire

TEST(Rules, PlainEventsLeaveTheAnimationFieldsOut) {
    const auto j = ToJson(Event{.type = EventType::Info, .text = "hello"});
    EXPECT_FALSE(j.contains("spell"));
    EXPECT_FALSE(j.contains("element"));
    EXPECT_FALSE(j.contains("path"));
}

TEST(Rules, AnimationFieldsSurviveJson) {
    Event e{.type = EventType::Spell, .actor = 1, .target = 2, .amount = 7, .text = "boom"};
    e.spell = "fireball";
    e.element = "fire";
    e.radiusFt = 20;
    e.path = {1, 2, 2, 3};
    const Event back = EventFromJson(ToJson(e));
    EXPECT_EQ(back.spell, "fireball");
    EXPECT_EQ(back.element, "fire");
    EXPECT_EQ(back.radiusFt, 20);
    EXPECT_EQ(back.path, e.path);
    EXPECT_EQ(back.amount, 7);
}
