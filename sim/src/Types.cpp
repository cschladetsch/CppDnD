#include "dnd/Types.hpp"

namespace dnd {

std::string_view ToString(DamageType t) {
    switch (t) {
        case DamageType::Slashing: return "slashing";
        case DamageType::Piercing: return "piercing";
        case DamageType::Bludgeoning: return "bludgeoning";
        case DamageType::Fire: return "fire";
        case DamageType::Radiant: return "radiant";
        case DamageType::Force: return "force";
        case DamageType::Thunder: return "thunder";
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

std::string_view ToString(Role r) {
    switch (r) {
        case Role::Player: return "player";
        case Role::DungeonMaster: return "dungeon master";
        case Role::Spectator: return "spectator";
    }
    return "?";
}

}  // namespace dnd
