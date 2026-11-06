#pragma once
#include <iosfwd>

#include "dnd/Controller.hpp"

namespace dnd {

// Text-prompt controller. Reads commands from `in`, so it can also be driven by a script.
// On end-of-input it asks the session to hand control to the AI so a run never hangs.
class HumanController : public IController {
public:
    HumanController(std::istream& in, std::ostream& out) : in_(in), out_(out) {}
    Action DecideAction(const TurnContext& ctx) override;
    std::unique_ptr<IController> Clone() const override { return std::make_unique<HumanController>(in_, out_); }
    std::string_view Kind() const override { return "Human"; }

private:
    void ShowTurn(const TurnContext& ctx) const;
    void ShowHelp() const;

    std::istream& in_;
    std::ostream& out_;
    int lastRound_ = -1;
    int lastActor_ = -1;
};

}  // namespace dnd
