#include "dnd/AI.hpp"

#include <algorithm>
#include <optional>

#include "dnd/Actor.hpp"
#include "dnd/Combat.hpp"
#include "dnd/Content.hpp"

namespace dnd {
namespace {

constexpr double kNoPlan = -1e9;

struct Plan {
    Action act;
    double score = kNoPlan;
    bool valid() const { return score > kNoPlan; }
};

void Consider(Plan& best, Action a, double s) {
    if (s > best.score) { best.act = std::move(a); best.score = s; }
}

Action MakeCast(const std::string& spell, int target) {
    Action a;
    a.type = ActionType::CastSpell;
    a.spellId = spell;
    a.targetId = target;
    return a;
}

Action MakeAttack(int target, int index) {
    Action a;
    a.type = ActionType::Attack;
    a.targetId = target;
    a.attackIndex = index;
    return a;
}

Action MakeMove(Pos p) {
    Action a;
    a.type = ActionType::Move;
    a.dest = p;
    return a;
}

Action MakeFeature(const std::string& id) {
    Action a;
    a.type = ActionType::UseFeature;
    a.featureId = id;
    return a;
}

double HitChance(int toHit, int ac, RollMode m) {
    const double p = std::clamp((21.0 - (ac - toHit)) / 20.0, 0.05, 0.95);
    if (m == RollMode::Advantage) return 1.0 - (1.0 - p) * (1.0 - p);
    if (m == RollMode::Disadvantage) return p * p;
    return p;
}

double FailChance(int dc, int saveBonus) { return std::clamp((dc - saveBonus - 1) / 20.0, 0.0, 1.0); }

bool CanCastNow(const GameActor& me, const Spell& sp) {
    const auto& s = me.sheet;
    if (sp.bonusAction ? !s.economy.bonusAction : !s.economy.action) return false;
    if (sp.concentration && !me.concentrating.empty()) return false;  // would drop the current spell
    return sp.level == 0 || s.LowestSlotAtLeast(sp.level) > 0;
}

int SlotFor(const GameActor& me, const Spell& sp) { return sp.level == 0 ? 0 : me.sheet.LowestSlotAtLeast(sp.level); }

bool Casts(const GameActor& g) { return !g.sheet.spells.empty(); }

double BestAttackAverage(const GameActor& g) {
    double best = 0;
    for (const auto& w : g.sheet.attacks) best = std::max(best, w.damage.Average() * std::max(1, g.sheet.attacksPerAction));
    return best;
}

int MaxDamageRangeFt(const GameActor& me) {
    int r = 0;
    for (const auto& w : me.sheet.attacks) r = std::max(r, w.rangeFt);
    for (const auto& id : me.sheet.spells) {
        const Spell* sp = Content::FindSpell(id);
        const bool damaging = sp && sp->kind != SpellKind::Heal && sp->kind != SpellKind::Buff;
        if (damaging && (sp->level == 0 || me.sheet.LowestSlotAtLeast(sp->level) > 0)) r = std::max(r, sp->rangeFt);
    }
    return r;
}

// Movement actually available for walking, after paying to stand up from prone.
int UsableMovement(const GameActor& me) {
    const int m = me.sheet.economy.movementFt;
    return me.HasCondition(Condition::Prone) ? m - me.SpeedFt() / 2 : m;
}

double TargetScore(const Combat&, const GameActor& me, const GameActor& t) {
    const int distCells = DistanceFt(me.pos, t.pos) / 5;
    const int intel = me.sheet.Score(Ability::Int);
    if (me.team == Team::Monsters && intel < 6) return -distCells;  // mindless: nearest

    double score = static_cast<double>(t.sheet.maxHp - t.sheet.hp) / (distCells + 1);
    score += 0.5 * BestAttackAverage(t) + (Casts(t) ? 2.0 : 0.0);  // threat level modifier
    if (me.team == Team::Monsters && intel >= 10) {                // cunning: go for soft targets
        if (Casts(t)) score += 6.0;
        if (t.sheet.armorClass <= 13) score += 2.0;
        if (t.sheet.maxHp <= 20) score += 2.0;
    }
    if (t.IsDown()) score -= 100.0;  // do not waste attacks on the already-fallen
    if (t.HasCondition(Condition::Paralyzed)) score += 3.0;  // free crits
    if (DistanceFt(me.pos, t.pos) > me.sheet.economy.movementFt + MaxDamageRangeFt(me)) score -= 5.0;
    return score - 0.01 * distCells;
}

// Best way to hit this one target right now (weapons and single-target spells).
Plan BestSingle(const Combat& c, const GameActor& me, const GameActor& t) {
    Plan best;
    const auto& s = me.sheet;
    for (std::size_t i = 0; i < s.attacks.size(); ++i) {
        const auto& w = s.attacks[i];
        const AttackPreview pv = c.PreviewWeapon(me, t, w);
        if (!pv.legal) continue;
        const double p = HitChance(w.toHit, t.EffectiveAc() + pv.coverBonus, pv.mode);
        double dmg = w.damage.Average() * (pv.autoCritOnHit ? 2.0 : 1.0) + (pv.sneak ? s.sneakAttackDice * 3.5 : 0.0);
        dmg = Combat::AdjustDamage(t, static_cast<int>(dmg), w.type, w.magical);
        Consider(best, MakeAttack(t.id, static_cast<int>(i)), p * dmg);
    }
    if (s.economy.attacksLeft > 0) return best;  // mid Attack action: no spells
    for (const auto& id : s.spells) {
        const Spell* sp = Content::FindSpell(id);
        if (!sp || sp->kind == SpellKind::Heal || sp->kind == SpellKind::Buff || sp->radiusFt > 0 || !CanCastNow(me, *sp)) continue;
        if (!sp->requiresTag.empty() && !t.sheet.HasTag(sp->requiresTag)) continue;
        if (DistanceFt(me.pos, t.pos) > sp->rangeFt || c.Grid().CoverBetween(me.pos, t.pos) == Cover::Total) continue;
        const int slot = SlotFor(me, *sp);
        const double avg = Combat::SpellDice(s, *sp, slot).Average();
        double ev = 0;
        switch (sp->kind) {
            case SpellKind::AttackRoll: {
                const AttackPreview pv = c.PreviewAttack(me, t, true, sp->rangeFt, sp->rangeFt, false);
                if (!pv.legal) continue;
                ev = HitChance(s.SpellAttackBonus(), t.EffectiveAc() + pv.coverBonus, pv.mode) * avg * (sp->grantsAdvantage ? 1.15 : 1.0);
                break;
            }
            case SpellKind::AutoHit: ev = Combat::SpellProjectiles(*sp, slot) * avg; break;
            case SpellKind::SaveNegates: ev = FailChance(s.SpellSaveDc(), t.sheet.SaveBonus(sp->save)) * avg; break;
            case SpellKind::SaveForHalf: {
                const double pf = FailChance(s.SpellSaveDc(), t.sheet.SaveBonus(sp->save));
                ev = avg * (pf + 0.5 * (1.0 - pf));
                break;
            }
            case SpellKind::Condition: {
                if (t.HasCondition(sp->condition) || t.IsDown()) continue;
                if (std::find(t.sheet.conditionImmune.begin(), t.sheet.conditionImmune.end(), sp->condition) != t.sheet.conditionImmune.end()) continue;
                // A paralyzed foe takes automatic crits and cannot act: worth about two rounds of its damage plus ours.
                ev = FailChance(s.SpellSaveDc(), t.sheet.SaveBonus(sp->save)) * (BestAttackAverage(t) * 1.5 + 6.0);
                break;
            }
            default: continue;
        }
        Consider(best, MakeCast(sp->id, t.id), ev - 2.0 * sp->level);  // slots are a resource
    }
    return best;
}

// Best burst placement: only worthwhile if it catches two or more foes; allies count against it.
Plan BestBurst(const Combat& c, const GameActor& me) {
    Plan best;
    const auto& s = me.sheet;
    for (const auto& id : s.spells) {
        const Spell* sp = Content::FindSpell(id);
        if (!sp || sp->radiusFt == 0 || sp->kind != SpellKind::SaveForHalf || !CanCastNow(me, *sp)) continue;
        const int slot = SlotFor(me, *sp);
        const double avg = Combat::SpellDice(s, *sp, slot).Average();
        for (const auto& centre : c.Actors()) {
            if (centre->IsDead() || DistanceFt(me.pos, centre->pos) > sp->rangeFt) continue;
            if (!c.Grid().HasLineOfSight(me.pos, centre->pos)) continue;
            double ev = 0;
            int foes = 0;
            for (const auto& x : c.Actors()) {
                if (x->IsDead() || !Combat::WithinBurst(centre->pos, x->pos, sp->radiusFt)) continue;
                if (!c.Grid().HasLineOfSight(centre->pos, x->pos)) continue;
                const double pf = FailChance(s.SpellSaveDc(), x->sheet.SaveBonus(sp->save));
                const double e = avg * (pf + 0.5 * (1.0 - pf));
                if (x->IsHostileTo(me)) { ev += e; if (!x->IsDown()) ++foes; }
                else ev -= 1.5 * e;
            }
            if (foes >= 2) Consider(best, MakeCast(sp->id, centre->id), ev - 2.0 * sp->level);
        }
    }
    return best;
}

// Cheapest reachable square from which `targetPos` is within `desiredRangeFt` and visible.
std::optional<Pos> ApproachCell(const Combat& c, const GameActor& me, Pos targetPos, int desiredRangeFt) {
    const auto& g = c.Grid();
    const auto pm = g.ComputePaths(me.pos, c.PassableFor(me));
    std::optional<Pos> best;
    int bestCost = kUnreachable, bestDist = 0;
    for (int y = 0; y < g.Height(); ++y) {
        for (int x = 0; x < g.Width(); ++x) {
            const Pos p{x, y};
            const int cost = pm.CostTo(p);
            if (cost == kUnreachable || cost == 0 || g.OccupantAt(p) >= 0) continue;
            const int d = DistanceFt(p, targetPos);
            if (d > desiredRangeFt || g.CoverBetween(p, targetPos) == Cover::Total) continue;
            if (cost < bestCost || (cost == bestCost && d > bestDist)) { best = p; bestCost = cost; bestDist = d; }
        }
    }
    return best;
}

// True if moving along `path` would actually get the creature somewhere: the affordable prefix,
// backed off so it never ends inside another creature (mirrors Combat::DoMove), is not empty.
bool CanMakeProgress(const Combat& c, const GameActor& me, const std::vector<Pos>& path) {
    const auto& g = c.Grid();
    const int mv = UsableMovement(me);
    std::size_t k = 0;
    int spent = 0;
    while (k < path.size() && spent + g.EnterCostFt(path[k]) <= mv) spent += g.EnterCostFt(path[k++]);
    while (k > 0 && g.OccupantAt(path[k - 1]) >= 0) --k;
    return k > 0;
}

std::optional<Action> TrySelfHeal(const GameActor& me) {
    const auto& s = me.sheet;
    if (s.economy.bonusAction && s.FeatureUses("second_wind") > 0) return MakeFeature("second_wind");
    if (s.economy.bonusAction && s.KnowsSpell("healing_word") && s.LowestSlotAtLeast(1)) return MakeCast("healing_word", me.id);
    if (s.economy.action && s.economy.attacksLeft == 0 && s.KnowsSpell("cure_wounds") && s.LowestSlotAtLeast(1)) return MakeCast("cure_wounds", me.id);
    if (s.economy.action && s.economy.attacksLeft == 0 && s.potions > 0) {
        Action a;
        a.type = ActionType::UsePotion;
        a.targetId = me.id;
        return a;
    }
    return std::nullopt;
}

std::optional<Action> TrySupport(const Combat& c, const GameActor& me) {
    const auto& s = me.sheet;
    for (const GameActor* ally : c.DownedAlliesOf(me)) {
        if (ally->life == LifeState::Dead) continue;
        const int dist = DistanceFt(me.pos, ally->pos);
        const bool los = c.Grid().HasLineOfSight(me.pos, ally->pos);
        if (s.economy.bonusAction && s.KnowsSpell("healing_word") && s.LowestSlotAtLeast(1) && dist <= 60 && los)
            return MakeCast("healing_word", ally->id);
        const bool noAttackYet = s.economy.attacksLeft == 0 && s.economy.action;
        const bool canTouch = noAttackYet && ((s.KnowsSpell("cure_wounds") && s.LowestSlotAtLeast(1)) || s.potions > 0);
        if (!canTouch) continue;
        if (dist <= 5) {
            if (s.KnowsSpell("cure_wounds") && s.LowestSlotAtLeast(1)) return MakeCast("cure_wounds", ally->id);
            Action a;
            a.type = ActionType::UsePotion;
            a.targetId = ally->id;
            return a;
        }
        if (UsableMovement(me) >= 5)
            if (auto cell = ApproachCell(c, me, ally->pos, 5))
                if (CanMakeProgress(c, me, c.Grid().ComputePaths(me.pos, c.PassableFor(me)).PathTo(*cell))) return MakeMove(*cell);
    }
    return std::nullopt;
}

bool EnemyNear(const Combat& c, const GameActor& me, int rangeFt) {
    for (const GameActor* h : c.HostilesOf(me, false))
        if (DistanceFt(me.pos, h->pos) <= rangeFt) return true;
    return false;
}

// Bonus-action openers: rage when a fight is imminent, shield of faith before contact.
std::optional<Action> TryBonusOpeners(const Combat& c, const GameActor& me) {
    const auto& s = me.sheet;
    if (!s.economy.bonusAction) return std::nullopt;
    if (s.FeatureUses("rage") > 0 && EnemyNear(c, me, UsableMovement(me) + 10)) {
        bool raging = false;
        for (const auto& e : me.effects) raging = raging || e.id == "rage";
        if (!raging) return MakeFeature("rage");
    }
    if (me.concentrating.empty() && EnemyNear(c, me, 120)) {
        for (const auto& id : s.spells) {
            const Spell* sp = Content::FindSpell(id);
            if (!sp || sp->kind != SpellKind::Buff || !CanCastNow(me, *sp)) continue;
            // Buff the most exposed melee ally (lowest AC with no active buff from us).
            const GameActor* pick = nullptr;
            for (const auto& a : c.Actors()) {
                if (a->team != me.team || !a->IsConscious() || DistanceFt(me.pos, a->pos) > sp->rangeFt) continue;
                bool buffed = false;
                for (const auto& e : a->effects) buffed = buffed || e.id == sp->id;
                if (buffed) continue;
                const bool frontline = !a->sheet.attacks.empty() && !a->sheet.attacks.front().IsRanged() && a->sheet.spells.empty();
                if (!pick || (frontline && a->sheet.maxHp > pick->sheet.maxHp)) pick = a.get();
            }
            if (pick) return MakeCast(sp->id, pick->id);
        }
    }
    return std::nullopt;
}

Action Offence(const Combat& c, const GameActor& me) {
    const auto& eco = me.sheet.economy;
    const bool canAct = eco.action || eco.attacksLeft > 0;
    // After dashing the action is spent, but the extra movement is still worth using.
    if (!canAct && !(me.dashedThisTurn && UsableMovement(me) >= 5)) return Action::End();

    const bool mindless = me.team == Team::Monsters && me.sheet.Score(Ability::Int) < 6;
    const auto hostiles = c.HostilesOf(me, true);
    const bool anyStanding = std::any_of(hostiles.begin(), hostiles.end(), [](const GameActor* h) { return !h->IsDown(); });
    std::vector<const GameActor*> targets;
    for (const GameActor* h : hostiles)
        if (!h->IsDown() || mindless || !anyStanding) targets.push_back(h);
    if (targets.empty()) return Action::End();

    std::vector<std::pair<double, const GameActor*>> ranked;
    for (const GameActor* t : targets) ranked.emplace_back(TargetScore(c, me, *t), t);
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& l, const auto& r) { return l.first > r.first; });

    if (canAct) {
        const Plan burst = BestBurst(c, me);
        // Compare the best attack on every target, favouring the preferred targets only mildly, so a
        // disadvantaged throw at a far foe never beats a clean melee swing at the adjacent one.
        Plan best;
        int rank = 0;
        for (const auto& [score, t] : ranked) {
            const Plan p = BestSingle(c, me, *t);
            if (p.valid()) Consider(best, p.act, p.score * (1.0 - 0.08 * rank));
            ++rank;
        }
        // Close in and swing: worth it when no foe is adjacent (so no opportunity attacks) and a
        // melee weapon would do better than what is available from here (e.g. a long-range throw).
        if (eco.action && eco.attacksLeft == 0 && !EnemyNear(c, me, 5) && UsableMovement(me) >= 5) {
            int r = 0;
            for (const auto& [score, t] : ranked) {
                if (r++ >= 3) break;
                double melee = 0;
                for (const auto& w : me.sheet.attacks)
                    if (!w.IsRanged()) melee = std::max(melee, HitChance(w.toHit, t->EffectiveAc(), RollMode::Normal) * w.damage.Average() * std::max(1, me.sheet.attacksPerAction));
                if (melee <= 0 || melee * 0.9 * (1.0 - 0.08 * (r - 1)) <= best.score) continue;
                const auto cell = ApproachCell(c, me, t->pos, 5);
                if (!cell) continue;
                const auto path = c.Grid().ComputePaths(me.pos, c.PassableFor(me)).PathTo(*cell);
                int cost = 0;
                for (const Pos& step : path) cost += c.Grid().EnterCostFt(step);
                if (!path.empty() && cost <= UsableMovement(me)) return MakeMove(*cell);
            }
        }
        if (best.valid() || burst.valid()) return (burst.valid() && burst.score > best.score) ? burst.act : best.act;
    }

    // Nothing hits from here: close the distance on the top-ranked target.
    const GameActor* top = ranked.front().second;
    const int desired = std::max(5, MaxDamageRangeFt(me));
    const auto cell = ApproachCell(c, me, top->pos, desired);
    if (!cell) return Action::End();
    // Only move if at least the first step is affordable (difficult terrain costs double).
    const auto path = c.Grid().ComputePaths(me.pos, c.PassableFor(me)).PathTo(*cell);
    if (CanMakeProgress(c, me, path)) return MakeMove(*cell);
    if (eco.action && !me.dashedThisTurn && eco.attacksLeft == 0) {
        Action a;
        a.type = ActionType::Dash;
        return a;
    }
    return Action::End();
}

}  // namespace

Action AIController::DecideAction(const TurnContext& ctx) {
    const GameActor& me = ctx.self;
    if (me.sheet.hp * 4 < me.sheet.maxHp || (me.sheet.hp * 2 < me.sheet.maxHp && me.sheet.FeatureUses("second_wind") > 0))
        if (auto a = TrySelfHeal(me)) return *a;
    if (auto a = TrySupport(ctx.combat, me)) return *a;
    if (auto a = TryBonusOpeners(ctx.combat, me)) return *a;
    return Offence(ctx.combat, me);
}

}  // namespace dnd
