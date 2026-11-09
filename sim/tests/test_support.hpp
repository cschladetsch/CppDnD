// Helpers shared by the test files.
#pragma once
#include "dnd/Content.hpp"

namespace dnd {

// A character from the game data, built at the given level.
inline CharacterSheet Hero(const char* id, int level = 5) { return Content::BuildCharacter(*Content::FindCharacter(id), level); }
// A monster's sheet from the game data.
inline CharacterSheet Mon(const char* id) { return Content::FindMonster(id)->sheet; }

}  // namespace dnd
