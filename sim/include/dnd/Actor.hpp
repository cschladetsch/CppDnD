#pragma once
#include <memory>
#include <string>

#include "dnd/Controller.hpp"
#include "dnd/Types.hpp"

namespace dnd {

enum class LifeState { Alive, Unconscious, Stable, Dead };
enum class DamageResult { Hurt, Dropped, DeathSaveFailed, Killed };

// A PC or monster on the board: sheet + position + whoever is steering it.
struct GameActor {
    int id = -1;
    Team team = Team::Party;
    CharacterSheet sheet;
    Pos pos;
    std::unique_ptr<IController> controller;

    LifeState life = LifeState::Alive;
    int deathSaveSuccesses = 0;
    int deathSaveFailures = 0;
    bool dodging = false;
    bool sneakUsedThisTurn = false;
    bool dashedThisTurn = false;

    const std::string& Name() const { return sheet.name; }
    bool IsConscious() const { return life == LifeState::Alive; }
    bool IsDown() const { return life == LifeState::Unconscious || life == LifeState::Stable; }
    bool IsDead() const { return life == LifeState::Dead; }
    bool IsHostileTo(const GameActor& o) const { return team != o.team; }

    // `critical` matters for downed targets: a crit is two death-save failures.
    DamageResult ApplyDamage(int amount, bool critical);
    int Heal(int amount);  // returns HP actually restored; revives a downed creature
};

}  // namespace dnd
