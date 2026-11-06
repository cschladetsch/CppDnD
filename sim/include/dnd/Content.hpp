#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "dnd/Types.hpp"

namespace dnd {

inline const DiceExpr kPotionOfHealing{2, 4, 2};

struct MonsterDef {
    std::string id;
    std::string name;
    std::string cr;
    int xp = 0;
    CharacterSheet sheet;  // template; name is overwritten per instance
};

namespace Content {

const Spell* FindSpell(std::string_view id);
const std::vector<MonsterDef>& Monsters();
const MonsterDef* FindMonster(std::string_view id);
std::vector<CharacterSheet> DefaultParty();  // level 3: fighter, wizard, cleric, rogue

}  // namespace Content
}  // namespace dnd
