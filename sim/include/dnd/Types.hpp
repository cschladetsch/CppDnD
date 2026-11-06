#pragma once
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "dnd/Dice.hpp"

namespace dnd {

enum class Ability { Str, Dex, Con, Int, Wis, Cha };

// floor((score - 10) / 2)
constexpr int AbilityModifier(int score) {
    const int d = score - 10;
    return d >= 0 ? d / 2 : -((-d + 1) / 2);
}

enum class DamageType { Slashing, Piercing, Bludgeoning, Fire, Radiant, Force, Thunder };
enum class Team { Party, Monsters };
enum class Role { Player, DungeonMaster, Spectator };

std::string_view ToString(DamageType);
std::string_view ToString(Ability);
std::string_view ToString(Role);

struct Pos {
    int x = 0;
    int y = 0;
    friend constexpr bool operator==(const Pos&, const Pos&) = default;
};

// 5e variant grid rule: diagonals cost the same as orthogonal steps.
constexpr int DistanceFt(Pos a, Pos b) {
    int dx = a.x - b.x, dy = a.y - b.y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return 5 * (dx > dy ? dx : dy);
}

struct WeaponAttack {
    std::string name;
    int toHit = 0;
    DiceExpr damage;
    DamageType type = DamageType::Slashing;
    int rangeFt = 5;       // reach for melee, normal range for ranged
    int longRangeFt = 0;   // 0 => melee
    bool finesse = false;  // melee weapon that qualifies for sneak attack
    bool IsRanged() const { return longRangeFt > 0; }
};

enum class SpellKind { AttackRoll, AutoHit, SaveNegates, SaveForHalf, Heal };

struct Spell {
    std::string id;
    std::string name;
    int level = 0;
    bool bonusAction = false;
    int rangeFt = 60;
    SpellKind kind = SpellKind::AttackRoll;
    DiceExpr dice;
    DamageType damageType = DamageType::Force;
    Ability save = Ability::Dex;
    int radiusFt = 0;  // > 0: burst centred on the target creature's square
    int projectiles = 1;
    int upcastDice = 0;
    int upcastProjectiles = 0;
    bool addsCastingMod = false;
};

struct ActionEconomy {
    bool action = true;
    bool bonusAction = true;
    bool reaction = true;
    int movementFt = 0;
    void Reset(int speedFt) {
        action = bonusAction = reaction = true;
        movementFt = speedFt;
    }
};

struct CharacterSheet {
    std::string name;
    int level = 1;
    std::array<int, 6> scores{10, 10, 10, 10, 10, 10};
    std::array<bool, 6> saveProficient{};
    int proficiency = 2;
    int armorClass = 10;
    int speedFt = 30;
    int maxHp = 1;
    int hp = 1;
    int tempHp = 0;

    Ability spellAbility = Ability::Int;
    std::array<int, 10> slotsMax{};  // index = slot level (1..9)
    std::array<int, 10> slots{};
    std::vector<std::string> spells;  // spell ids

    std::vector<WeaponAttack> attacks;
    int potions = 0;
    int sneakAttackDice = 0;
    int xpValue = 0;  // monsters

    ActionEconomy economy;

    int Score(Ability a) const { return scores[static_cast<std::size_t>(a)]; }
    int Mod(Ability a) const { return AbilityModifier(Score(a)); }
    int SaveBonus(Ability a) const {
        return Mod(a) + (saveProficient[static_cast<std::size_t>(a)] ? proficiency : 0);
    }
    int SpellAttackBonus() const { return proficiency + Mod(spellAbility); }
    int SpellSaveDc() const { return 8 + SpellAttackBonus(); }
    bool KnowsSpell(std::string_view id) const {
        for (const auto& s : spells)
            if (s == id) return true;
        return false;
    }
    // Lowest slot level >= level with a slot remaining, or 0.
    int LowestSlotAtLeast(int level) const {
        for (int l = level; l <= 9; ++l)
            if (slots[static_cast<std::size_t>(l)] > 0) return l;
        return 0;
    }
};

}  // namespace dnd
