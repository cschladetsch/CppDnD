#pragma once
#include <memory>
#include <string>
#include <string_view>

#include "dnd/Types.hpp"

namespace dnd {

class Combat;
struct GameActor;

enum class ActionType { Move, Attack, CastSpell, Dash, Dodge, UsePotion, SwapRole, EndTurn };

struct Action {
    ActionType type = ActionType::EndTurn;
    int targetId = -1;
    Pos dest{};
    int attackIndex = -1;  // -1: engine picks the first attack that can reach
    std::string spellId;
    int slotLevel = 0;     // 0: lowest available
    Role role = Role::Spectator;  // for SwapRole

    static Action End() { return Action{}; }
};

// Read-only view of the world handed to a controller when it must decide.
struct TurnContext {
    const Combat& combat;
    const GameActor& self;
};

// Anything that can drive an actor (or the DM). Swap at runtime to change who is "playing".
class IController {
public:
    virtual ~IController() = default;
    virtual Action DecideAction(const TurnContext& ctx) = 0;
    virtual std::unique_ptr<IController> Clone() const = 0;
    virtual std::string_view Kind() const = 0;
};

}  // namespace dnd
