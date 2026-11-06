#pragma once
#include "dnd/Controller.hpp"

namespace dnd {

// Utility-scored tactical AI. Used for AI party members and for monsters; monster behaviour
// is shaped by INT (< 6 attacks the nearest creature, >= 10 hunts casters and soft targets).
//   1. Survival: below 25% HP, heal self (Healing Word, Cure Wounds, potion)
//   2. Support : revive a downed ally
//   3. Offence : target score = (maxHP - HP) / (dist + 1) + threat; attack, cast, or close in
class AIController : public IController {
public:
    Action DecideAction(const TurnContext& ctx) override;
    std::unique_ptr<IController> Clone() const override { return std::make_unique<AIController>(); }
    std::string_view Kind() const override { return "AI"; }
};

}  // namespace dnd
