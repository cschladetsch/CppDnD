#pragma once
#include <memory>
#include <string>
#include <vector>

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
    std::vector<Effect> effects;
    std::string concentrating;  // spell id, empty if none

    const std::string& Name() const { return sheet.name; }
    bool IsConscious() const { return life == LifeState::Alive; }
    bool IsDown() const { return life == LifeState::Unconscious || life == LifeState::Stable; }
    bool IsDead() const { return life == LifeState::Dead; }
    bool IsHostileTo(const GameActor& o) const { return team != o.team; }

    bool HasCondition(Condition c) const {
        for (const auto& e : effects)
            if (e.condition == c) return true;
        return false;
    }
    bool IsIncapacitated() const {
        return HasCondition(Condition::Incapacitated) || HasCondition(Condition::Paralyzed) || HasCondition(Condition::Stunned) || IsDown();
    }
    int EffectiveAc() const {
        int ac = sheet.armorClass;
        for (const auto& e : effects) ac += e.acBonus;
        return ac;
    }
    int SpeedFt() const {
        if (HasCondition(Condition::Restrained) || HasCondition(Condition::Paralyzed) || HasCondition(Condition::Stunned)) return 0;
        if (sheet.exhaustion >= 5) return 0;
        return sheet.exhaustion >= 2 ? sheet.speedFt / 2 : sheet.speedFt;
    }

    // `critical` matters for downed targets: a crit is two death-save failures.
    DamageResult ApplyDamage(int amount, bool critical);
    int Heal(int amount);  // returns HP actually restored; revives a downed creature
};

}  // namespace dnd
