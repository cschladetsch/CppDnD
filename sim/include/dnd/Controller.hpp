#pragma once
#include <memory>
#include <string>
#include <string_view>

#include "dnd/Types.hpp"

namespace dnd {

class Combat;
struct GameActor;

// AwaitInput means "a remote human must answer"; Combat::Advance() then returns to its caller.
enum class ActionType { Move, Attack, CastSpell, Dash, Dodge, UsePotion, UseFeature, SwapRole, EndTurn, AwaitInput };

struct Action {
    ActionType type = ActionType::EndTurn;
    int targetId = -1;
    Pos dest{};
    int attackIndex = -1;  // -1: engine picks the first attack that can reach
    std::string spellId;
    std::string featureId;  // rage, second_wind
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
    // Called after the engine resolved (or refused) the action this controller returned.
    virtual void OnActionResult(const TurnContext&, const Action&, bool ok, const std::string& reason) {}
};

// Controller for a human reached over the network: it never answers by itself. Combat::Advance()
// pauses on it and resumes when the server calls Combat::Submit().
class RemoteController : public IController {
public:
    Action DecideAction(const TurnContext&) override {
        Action a;
        a.type = ActionType::AwaitInput;
        return a;
    }
    std::unique_ptr<IController> Clone() const override { return std::make_unique<RemoteController>(); }
    std::string_view Kind() const override { return "Remote"; }
};

}  // namespace dnd
