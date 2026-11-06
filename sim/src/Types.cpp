#include "dnd/Types.hpp"

#include <algorithm>
#include <cctype>

namespace dnd {
namespace {
std::string Lower(std::string_view s) {
    std::string t(s);
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return t;
}
}  // namespace

std::string_view ToString(DamageType t) {
    switch (t) {
        case DamageType::Slashing: return "slashing";
        case DamageType::Piercing: return "piercing";
        case DamageType::Bludgeoning: return "bludgeoning";
        case DamageType::Fire: return "fire";
        case DamageType::Radiant: return "radiant";
        case DamageType::Force: return "force";
        case DamageType::Thunder: return "thunder";
        case DamageType::Poison: return "poison";
        case DamageType::Necrotic: return "necrotic";
        case DamageType::Cold: return "cold";
        case DamageType::Lightning: return "lightning";
        case DamageType::Acid: return "acid";
        case DamageType::Psychic: return "psychic";
    }
    return "?";
}

std::string_view ToString(Ability a) {
    switch (a) {
        case Ability::Str: return "STR";
        case Ability::Dex: return "DEX";
        case Ability::Con: return "CON";
        case Ability::Int: return "INT";
        case Ability::Wis: return "WIS";
        case Ability::Cha: return "CHA";
    }
    return "?";
}

std::string_view ToString(Condition c) {
    switch (c) {
        case Condition::None: return "none";
        case Condition::Blinded: return "blinded";
        case Condition::Frightened: return "frightened";
        case Condition::Incapacitated: return "incapacitated";
        case Condition::Paralyzed: return "paralyzed";
        case Condition::Poisoned: return "poisoned";
        case Condition::Prone: return "prone";
        case Condition::Restrained: return "restrained";
        case Condition::Stunned: return "stunned";
    }
    return "?";
}

std::string_view ToString(Role r) {
    switch (r) {
        case Role::Player: return "player";
        case Role::DungeonMaster: return "dungeon master";
        case Role::Spectator: return "spectator";
    }
    return "?";
}

std::optional<DamageType> ParseDamageType(std::string_view s) {
    const auto t = Lower(s);
    for (int i = 0; i <= static_cast<int>(DamageType::Psychic); ++i)
        if (ToString(static_cast<DamageType>(i)) == t) return static_cast<DamageType>(i);
    return std::nullopt;
}

std::optional<Ability> ParseAbility(std::string_view s) {
    const auto t = Lower(s);
    const char* shortNames[] = {"str", "dex", "con", "int", "wis", "cha"};
    const char* longNames[] = {"strength", "dexterity", "constitution", "intelligence", "wisdom", "charisma"};
    for (int i = 0; i < 6; ++i)
        if (t == shortNames[i] || t == longNames[i]) return static_cast<Ability>(i);
    return std::nullopt;
}

std::optional<Condition> ParseCondition(std::string_view s) {
    const auto t = Lower(s);
    for (int i = 0; i <= static_cast<int>(Condition::Stunned); ++i)
        if (ToString(static_cast<Condition>(i)) == t) return static_cast<Condition>(i);
    return std::nullopt;
}

std::optional<Role> ParseRole(std::string_view s) {
    const auto t = Lower(s);
    if (t == "player") return Role::Player;
    if (t == "dm" || t == "dungeon master" || t == "dungeonmaster") return Role::DungeonMaster;
    if (t == "auto" || t == "spectator") return Role::Spectator;
    return std::nullopt;
}

}  // namespace dnd
