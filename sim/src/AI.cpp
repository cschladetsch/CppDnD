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
    return sp.level == 0 || s.LowestSlotAtLeast(sp.level) > 0;
}

int SlotFor(const GameActor& me, const Spell& sp) { return sp.level == 0 ? 0 : me.sheet.LowestSlotAtLeast(sp.level); }

bool Casts(const GameActor& g) { return !g.sheet.spells.empty(); }

double BestAttackAverage(const GameActor& g) {
    double best = 0;
    for (const auto& w : g.sheet.attacks) best = std::max(best, w.damage.Average());
    return best;
}

int MaxDamageRangeFt(const GameActor& me) {
    int r = 0;
    for (const auto& w : me.sheet.attacks) r = std::max(r, w.rangeFt);
    for (const auto& id : me.sheet.spells) {
        const Spell* sp = Content::FindSpell(id);
        if (sp && sp->kind != SpellKind::Heal && CanCastNow(me, *sp)) r = std::max(r, sp->rangeFt);
    }
    return r;
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
        const double p = HitChance(w.toHit, t.sheet.armorClass + pv.coverBonus, pv.mode);
        const double dmg = w.damage.Average() * (pv.autoCritOnHit ? 2.0 : 1.0) + (pv.sneak ? s.sneakAttackDice * 3.5 : 0.0);
        Consider(best, MakeAttack(t.id, static_cast<int>(i)), p * dmg);
    }
    for (const auto& id : s.spells) {
        const Spell* sp = Content::FindSpell(id);
        if (!sp || sp->kind == SpellKind::Heal || sp->radiusFt > 0 || !CanCastNow(me, *sp)) continue;
        if (DistanceFt(me.pos, t.pos) > sp->rangeFt || c.Grid().CoverBetween(me.pos, t.pos) == Cover::Total) continue;
        const int slot = SlotFor(me, *sp);
        const double avg = Combat::SpellDice(s, *sp, slot).Average();
        double ev = 0;
        switch (sp->kind) {
            case SpellKind::AttackRoll: {
                const AttackPreview pv = c.PreviewAttack(me, t, true, sp->rangeFt, sp->rangeFt, false);
                if (!pv.legal) continue;
                ev = HitChance(s.SpellAttackBonus(), t.sheet.armorClass + pv.coverBonus, pv.mode) * avg;
                break;
            }
            case SpellKind::AutoHit: ev = Combat::SpellProjectiles(*sp, slot) * avg; break;
            case SpellKind::SaveNegates: ev = FailChance(s.SpellSaveDc(), t.sheet.SaveBonus(sp->save)) * avg; break;
            case SpellKind::SaveForHalf: {
                const double pf = FailChance(s.SpellSaveDc(), t.sheet.SaveBonus(sp->save));
                ev = avg * (pf + 0.5 * (1.0 - pf));
                break;
            }
            case SpellKind::Heal: continue;
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
        if (!sp || sp->radiusFt == 0 || sp->kind == SpellKind::Heal || !CanCastNow(me, *sp)) continue;
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

std::optional<Action> TrySelfHeal(const GameActor& me) {
    const auto& s = me.sheet;
    if (s.economy.bonusAction && s.KnowsSpell("healing_word") && s.LowestSlotAtLeast(1)) return MakeCast("healing_word", me.id);
    if (s.economy.action && s.KnowsSpell("cure_wounds") && s.LowestSlotAtLeast(1)) return MakeCast("cure_wounds", me.id);
    if (s.economy.action && s.potions > 0) {
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
        const bool canTouch = (s.economy.action && s.KnowsSpell("cure_wounds") && s.LowestSlotAtLeast(1)) ||
                              (s.economy.action && s.potions > 0);
        if (!canTouch) continue;
        if (dist <= 5) {
            if (s.KnowsSpell("cure_wounds") && s.LowestSlotAtLeast(1)) return MakeCast("cure_wounds", ally->id);
            Action a;
            a.type = ActionType::UsePotion;
            a.targetId = ally->id;
            return a;
        }
        if (s.economy.movementFt >= 5)
            if (auto cell = ApproachCell(c, me, ally->pos, 5)) return MakeMove(*cell);
    }
    return std::nullopt;
}

Action Offence(const Combat& c, const GameActor& me) {
    const auto& eco = me.sheet.economy;
    const bool canAct = eco.action;
    // After dashing the action is spent, but the extra movement is still worth using.
    if (!canAct && !(me.dashedThisTurn && eco.movementFt >= 5)) return Action::End();

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
        for (const auto& [score, t] : ranked) {
            const Plan p = BestSingle(c, me, *t);
            if (!p.valid()) continue;
            return (burst.valid() && burst.score > p.score) ? burst.act : p.act;
        }
        if (burst.valid()) return burst.act;
    }

    // Nothing hits from here: close the distance on the top-ranked target.
    const GameActor* top = ranked.front().second;
    const int desired = std::max(5, MaxDamageRangeFt(me));
    const auto cell = ApproachCell(c, me, top->pos, desired);
    if (!cell) return Action::End();
    // Only move if at least the first step is affordable (difficult terrain costs double).
    const auto path = c.Grid().ComputePaths(me.pos, c.PassableFor(me)).PathTo(*cell);
    if (!path.empty() && c.Grid().EnterCostFt(path.front()) <= eco.movementFt && c.Grid().OccupantAt(path.front()) < 0)
        return MakeMove(*cell);
    if (canAct && !me.dashedThisTurn) {
        Action a;
        a.type = ActionType::Dash;
        return a;
    }
    return Action::End();
}

}  // namespace

Action AIController::DecideAction(const TurnContext& ctx) {
    const GameActor& me = ctx.self;
    if (me.sheet.hp * 4 < me.sheet.maxHp)
        if (auto a = TrySelfHeal(me)) return *a;
    if (auto a = TrySupport(ctx.combat, me)) return *a;
    return Offence(ctx.combat, me);
}

}  // namespace dnd
