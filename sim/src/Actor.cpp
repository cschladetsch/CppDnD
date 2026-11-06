#include "dnd/Actor.hpp"

#include <algorithm>

namespace dnd {

DamageResult GameActor::ApplyDamage(int amount, bool critical) {
    if (life == LifeState::Dead) return DamageResult::Killed;
    if (amount <= 0) return DamageResult::Hurt;

    if (IsDown()) {
        if (amount >= sheet.maxHp) { life = LifeState::Dead; return DamageResult::Killed; }
        life = LifeState::Unconscious;  // damage ends stability
        deathSaveFailures += critical ? 2 : 1;
        if (deathSaveFailures >= 3) { life = LifeState::Dead; return DamageResult::Killed; }
        return DamageResult::DeathSaveFailed;
    }

    const int absorbed = std::min(sheet.tempHp, amount);
    sheet.tempHp -= absorbed;
    amount -= absorbed;
    sheet.hp -= amount;
    if (sheet.hp > 0) return DamageResult::Hurt;

    const int overflow = -sheet.hp;
    sheet.hp = 0;
    if (team == Team::Monsters || overflow >= sheet.maxHp) {  // monsters die; PCs die on massive damage
        life = LifeState::Dead;
        return DamageResult::Killed;
    }
    life = LifeState::Unconscious;
    deathSaveSuccesses = deathSaveFailures = 0;
    return DamageResult::Dropped;
}

int GameActor::Heal(int amount) {
    if (life == LifeState::Dead || amount <= 0) return 0;
    const int before = sheet.hp;
    sheet.hp = std::min(sheet.maxHp, sheet.hp + amount);
    if (sheet.hp > 0 && life != LifeState::Alive) {
        life = LifeState::Alive;
        deathSaveSuccesses = deathSaveFailures = 0;
    }
    return sheet.hp - before;
}

}  // namespace dnd
