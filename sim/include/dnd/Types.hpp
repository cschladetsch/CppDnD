#pragma once
#include <array>
#include <map>
#include <optional>
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

enum class DamageType {
    Slashing, Piercing, Bludgeoning, Fire, Radiant, Force, Thunder,
    Poison, Necrotic, Cold, Lightning, Acid, Psychic
};
constexpr bool IsPhysical(DamageType t) {
    return t == DamageType::Slashing || t == DamageType::Piercing || t == DamageType::Bludgeoning;
}

// SRD 5.1 conditions that change how the engine resolves rolls. (Unconscious is a LifeState.)
enum class Condition { None, Blinded, Frightened, Incapacitated, Paralyzed, Poisoned, Prone, Restrained, Stunned };

enum class Team { Party, Monsters };
enum class Role { Player, DungeonMaster, Spectator };

std::string_view ToString(DamageType);
std::string_view ToString(Ability);
std::string_view ToString(Condition);
std::string_view ToString(Role);
std::optional<DamageType> ParseDamageType(std::string_view);
std::optional<Ability> ParseAbility(std::string_view);  // "str", "STR", "strength"
std::optional<Condition> ParseCondition(std::string_view);
std::optional<Role> ParseRole(std::string_view);

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

// Extra effect applied to a creature hit by an attack (ghoul claws, wolf bite).
struct AttackRider {
    bool present = false;
    Ability save = Ability::Con;
    int dc = 10;
    Condition condition = Condition::None;
    int rounds = 10;  // 1 minute
    bool saveEnds = false;
    std::vector<std::string> exceptTags;  // creatures with any of these tags are unaffected
};

struct WeaponAttack {
    std::string name;
    int toHit = 0;
    DiceExpr damage;
    DamageType type = DamageType::Slashing;
    int rangeFt = 5;       // reach for melee, normal range for ranged
    int longRangeFt = 0;   // 0 => melee
    bool finesse = false;  // melee weapon that qualifies for sneak attack
    bool magical = false;  // bypasses "nonmagical weapon" resistance
    AttackRider rider;
    bool IsRanged() const { return longRangeFt > 0; }
};

enum class SpellKind { AttackRoll, AutoHit, SaveNegates, SaveForHalf, Heal, Condition, Buff };

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
    bool concentration = false;
    Condition condition = Condition::None;  // SpellKind::Condition
    int durationRounds = 10;
    bool saveEnds = false;
    int acBonus = 0;                        // SpellKind::Buff
    bool grantsAdvantage = false;           // guiding bolt rider
    std::string requiresTag;                // e.g. "humanoid" for hold person
};

// A lasting effect on a creature: a condition, a buff, or a rider like rage.
struct Effect {
    std::string id;
    std::string name;
    Condition condition = Condition::None;
    int rounds = -1;  // -1: until removed
    int sourceId = -1;
    bool concentration = false;  // lasts only while sourceId keeps concentrating
    bool saveEnds = false;       // repeat the save at the end of the holder's turns
    Ability saveAbility = Ability::Wis;
    int saveDc = 10;
    int acBonus = 0;
    int meleeDamageBonus = 0;
    bool resistPhysical = false;
    bool advantageAgainst = false;   // next attack against the holder has advantage
    bool expireAtSourceTurn = false; // removed when the source's next turn begins
};

struct ActionEconomy {
    bool action = true;
    bool bonusAction = true;
    bool reaction = true;
    int movementFt = 0;
    int attacksLeft = 0;  // >0 while an Attack action is in progress (Extra Attack)
    void Reset(int speedFt) {
        action = bonusAction = reaction = true;
        movementFt = speedFt;
        attacksLeft = 0;
    }
};

struct CharacterSheet {
    std::string name;
    std::string classId;                // empty for monsters
    std::string monsterId;              // monsters only: the id in monsters.json (agents.json can pick a model by it)
    std::vector<std::string> tags;      // creature type + race, e.g. "humanoid", "elf", "undead"
    int level = 1;
    std::array<int, 6> scores{10, 10, 10, 10, 10, 10};
    std::array<bool, 6> saveProficient{};
    int proficiency = 2;
    int armorClass = 10;
    int speedFt = 30;
    int maxHp = 1;
    int hp = 1;
    int tempHp = 0;
    int hitDie = 8;
    int hitDiceLeft = 1;
    int exhaustion = 0;

    Ability spellAbility = Ability::Int;
    std::array<int, 10> slotsMax{};  // index = slot level (1..9)
    std::array<int, 10> slots{};
    std::vector<std::string> spells;  // spell ids

    std::vector<WeaponAttack> attacks;
    int attacksPerAction = 1;
    int potions = 0;
    int sneakAttackDice = 0;
    int xpValue = 0;  // monsters

    std::vector<DamageType> resist, vulnerable, immune;
    std::vector<Condition> conditionImmune;
    bool resistNonmagicalPhysical = false;
    std::map<std::string, int> traits;       // pack_tactics, regeneration:10, ...
    std::map<std::string, int> featureUses;  // remaining uses of limited features
    std::map<std::string, int> featureMax;

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
    bool HasTag(std::string_view t) const {
        for (const auto& s : tags)
            if (s == t) return true;
        return false;
    }
    int Trait(const std::string& k) const {
        auto it = traits.find(k);
        return it == traits.end() ? 0 : it->second;
    }
    int FeatureUses(const std::string& k) const {
        auto it = featureUses.find(k);
        return it == featureUses.end() ? 0 : it->second;
    }
    // Lowest slot level >= level with a slot remaining, or 0.
    int LowestSlotAtLeast(int level) const {
        for (int l = level; l <= 9; ++l)
            if (slots[static_cast<std::size_t>(l)] > 0) return l;
        return 0;
    }
};

}  // namespace dnd
