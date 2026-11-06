#pragma once
#include <iosfwd>
#include <memory>
#include <string>

#include "dnd/Combat.hpp"
#include "dnd/DM.hpp"
#include "dnd/Events.hpp"

namespace dnd {

// Owns the table: party, DM engine, the current fight, and the live role assignment.
//   Role::Player          human drives one hero; AI drives the rest of the party and the monsters
//   Role::DungeonMaster   human drives every monster; AI drives the whole party
//   Role::Spectator       AI drives everything
// The role can be swapped mid-fight (the `swap` command), which re-assigns controllers in place.
class Session {
public:
    Session(std::istream& in, std::ostream& out, std::uint64_t seed);

    void SetRole(Role r);
    Role GetRole() const { return role_; }
    void SetHero(std::string name) { hero_ = std::move(name); }
    void SetQuiet(bool q) { quiet_ = q; }

    CombatResult RunEncounter(Difficulty d);

private:
    static TacticalGrid BuildRoom();
    void AssignControllers();
    void OnRoleSwap(Role r);
    void OnEvent(const Event& e);

    std::istream& in_;
    std::ostream& out_;
    Role role_ = Role::Spectator;
    std::string hero_ = "Thorin";
    bool quiet_ = false;
    EventBus bus_;
    DungeonMasterEngine dm_;
    std::unique_ptr<Combat> combat_;
};

}  // namespace dnd
