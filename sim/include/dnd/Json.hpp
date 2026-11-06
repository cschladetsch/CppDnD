#pragma once
#include <string>

#include <nlohmann/json.hpp>

#include "dnd/Combat.hpp"
#include "dnd/Controller.hpp"
#include "dnd/Events.hpp"
#include "dnd/Types.hpp"

namespace dnd {

using Json = nlohmann::json;

// Lossless (round-trip) serialization used by save files and the HTTP API.
Json ToJson(const DiceExpr&);
DiceExpr DiceFromJson(const Json&);
Json ToJson(const CharacterSheet&);
CharacterSheet SheetFromJson(const Json&);
Json ToJson(const Effect&);
Effect EffectFromJson(const Json&);
Json ToJson(const Event&);
Event EventFromJson(const Json&);

// Action <-> API payload. FromJson returns an error message in `err` and a default Action on failure.
Json ToJson(const Action&);
bool ActionFromJson(const Json&, Action& out, std::string& err);

// Read-only snapshot of a fight for a client: map rows, combatants, whose turn it is and, for the
// actor being awaited, the legal options. Never leaks anything a player could not see.
Json CombatView(const Combat&);

}  // namespace dnd
