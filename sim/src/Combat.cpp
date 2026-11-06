#include "dnd/Combat.hpp"

#include <algorithm>
#include <sstream>

#include "dnd/Content.hpp"
#include "dnd/Util.hpp"

namespace dnd {
namespace {

ActionResult Fail(std::string why) { return ActionResult{false, std::move(why)}; }
int CantripExtraDice(int level) { return (level >= 5 ? 1 : 0) + (level >= 11 ? 1 : 0) + (level >= 17 ? 1 : 0); }

const char* ModeTag(RollMode m) {
    return m == RollMode::Advantage ? " (adv)" : m == RollMode::Disadvantage ? " (dis)" : "";
}

}  // namespace

Combat::Combat(TacticalGrid grid, EventBus& bus) : grid_(std::move(grid)), bus_(bus) {}

GameActor& Combat::AddActor(Team team, CharacterSheet sheet, Pos pos, std::unique_ptr<IController> controller) {
    auto a = std::make_unique<GameActor>();
    a->id = static_cast<int>(actors_.size());
    a->team = team;
    a->sheet = std::move(sheet);
    a->pos = pos;
    a->controller = std::move(controller);
    grid_.SetOccupant(pos, a->id);
    actors_.push_back(std::move(a));
    return *actors_.back();
}

GameActor* Combat::Find(int id) {
    return id >= 0 && id < static_cast<int>(actors_.size()) ? actors_[static_cast<std::size_t>(id)].get() : nullptr;
}
const GameActor* Combat::Find(int id) const {
    return id >= 0 && id < static_cast<int>(actors_.size()) ? actors_[static_cast<std::size_t>(id)].get() : nullptr;
}

void Combat::Say(EventType t, int actor, int target, std::string text, int roll, int vs, int amount) const {
    bus_.Emit(Event{t, actor, target, roll, vs, amount, std::move(text)});
}

void Combat::RemoveFromGrid(GameActor& a) {
    if (grid_.OccupantAt(a.pos) == a.id) grid_.SetOccupant(a.pos, -1);
}

// ---------------------------------------------------------------- queries

std::vector<const GameActor*> Combat::HostilesOf(const GameActor& self, bool includeDowned) const {
    std::vector<const GameActor*> v;
    for (const auto& p : actors_) {
        if (!self.IsHostileTo(*p) || p->IsDead()) continue;
        if (p->IsDown() && !includeDowned) continue;
        v.push_back(p.get());
    }
    return v;
}

std::vector<const GameActor*> Combat::DownedAlliesOf(const GameActor& self) const {
    std::vector<const GameActor*> v;
    for (const auto& p : actors_)
        if (p->team == self.team && p->IsDown()) v.push_back(p.get());
    std::sort(v.begin(), v.end(), [&](const GameActor* l, const GameActor* r) {
        return DistanceFt(self.pos, l->pos) < DistanceFt(self.pos, r->pos);
    });
    return v;
}

std::function<bool(Pos)> Combat::PassableFor(const GameActor& mover) const {
    const int moverTeam = static_cast<int>(mover.team);
    return [this, moverTeam](Pos p) {
        const int occ = grid_.OccupantAt(p);
        return occ < 0 || static_cast<int>(actors_[static_cast<std::size_t>(occ)]->team) == moverTeam;
    };
}

AttackPreview Combat::PreviewAttack(const GameActor& a, const GameActor& t, bool ranged, int rangeFt,
                                    int longRangeFt, bool sneakCapable) const {
    AttackPreview p;
    p.distanceFt = DistanceFt(a.pos, t.pos);
    auto refuse = [&](const char* why) { p.whyNot = why; return p; };

    if (t.IsDead()) return refuse("target is dead");
    if (!a.IsHostileTo(t)) return refuse("target is not hostile");
    if (p.distanceFt > (ranged ? longRangeFt : rangeFt)) return refuse("target is out of range");
    p.cover = grid_.CoverBetween(a.pos, t.pos);
    if (p.cover == Cover::Total) return refuse("no line of sight");
    p.coverBonus = p.cover == Cover::Half ? 2 : p.cover == Cover::ThreeQuarters ? 5 : 0;

    bool adv = false, dis = false;
    if (t.dodging && t.IsConscious()) dis = true;
    if (t.IsDown()) {
        adv = true;
        p.autoCritOnHit = p.distanceFt <= 5;
    }
    if (ranged) {
        if (p.distanceFt > rangeFt) dis = true;
        for (const auto& o : actors_)
            if (a.IsHostileTo(*o) && o->IsConscious() && DistanceFt(a.pos, o->pos) <= 5) { dis = true; break; }
    }
    p.mode = CombineModes(adv, dis);

    if (sneakCapable && a.sheet.sneakAttackDice > 0 && !a.sneakUsedThisTurn && p.mode != RollMode::Disadvantage) {
        bool allyAdjacent = false;
        for (const auto& o : actors_)
            if (o->id != a.id && o->team == a.team && o->IsConscious() && DistanceFt(o->pos, t.pos) <= 5) {
                allyAdjacent = true;
                break;
            }
        p.sneak = p.mode == RollMode::Advantage || allyAdjacent;
    }
    p.legal = true;
    return p;
}

AttackPreview Combat::PreviewWeapon(const GameActor& a, const GameActor& t, const WeaponAttack& w) const {
    return PreviewAttack(a, t, w.IsRanged(), w.rangeFt, w.longRangeFt, w.finesse || w.IsRanged());
}

DiceExpr Combat::SpellDice(const CharacterSheet& s, const Spell& sp, int slot) {
    const int extra = sp.level == 0 ? CantripExtraDice(s.level) : std::max(0, slot - sp.level) * sp.upcastDice;
    return sp.dice.WithExtraDice(extra);
}

int Combat::SpellProjectiles(const Spell& sp, int slot) {
    return sp.projectiles + (sp.level > 0 ? std::max(0, slot - sp.level) * sp.upcastProjectiles : 0);
}

bool Combat::WithinBurst(Pos c, Pos p, int radiusFt) {
    const int r = radiusFt / 5, dx = c.x - p.x, dy = c.y - p.y;
    return dx * dx + dy * dy <= r * r;
}

char Combat::Glyph(const GameActor& a) const {
    int rank = 0;
    for (const auto& o : actors_) {
        if (o->id == a.id) break;
        if (o->team == a.team) ++rank;
    }
    if (a.team == Team::Party) return static_cast<char>((a.IsConscious() ? 'A' : 'a') + rank);
    return rank < 9 ? static_cast<char>('1' + rank) : '*';
}

std::string Combat::RenderMap() const {
    std::ostringstream o;
    o << "    ";
    for (int x = 0; x < grid_.Width(); ++x) o << x % 10;
    o << '\n';
    for (int y = 0; y < grid_.Height(); ++y) {
        o << (y < 10 ? " " : "") << y << "  ";
        for (int x = 0; x < grid_.Width(); ++x) {
            const Pos p{x, y};
            const int occ = grid_.OccupantAt(p);
            char c = '.';
            switch (grid_.At(p)) {
                case Terrain::Wall: c = '#'; break;
                case Terrain::Difficult: c = '~'; break;
                case Terrain::HalfCover: c = 'c'; break;
                case Terrain::ThreeQuarterCover: c = 'O'; break;
                default: break;
            }
            if (occ >= 0) c = Glyph(*actors_[static_cast<std::size_t>(occ)]);
            o << c;
        }
        o << '\n';
    }
    o << "    legend: # wall, c half cover, O 3/4 cover, ~ difficult, lowercase = downed\n";
    for (const auto& a : actors_) {
        if (a->IsDead()) continue;
        o << "    " << Glyph(*a) << " [" << a->id << "] " << a->Name() << "  HP " << a->sheet.hp << "/" << a->sheet.maxHp
          << "  AC " << a->sheet.armorClass << "  @(" << a->pos.x << "," << a->pos.y << ")"
          << (a->IsDown() ? "  DOWN" : "") << '\n';
    }
    return o.str();
}

// ---------------------------------------------------------------- main loop

void Combat::RollInitiative() {
    struct Entry { int id, total, dex; };
    std::vector<Entry> v;
    for (const auto& a : actors_) {
        const int dex = a->sheet.Mod(Ability::Dex);
        v.push_back({a->id, DiceRoller::D20().natural + dex, dex});
    }
    std::stable_sort(v.begin(), v.end(), [](const Entry& l, const Entry& r) {
        return l.total != r.total ? l.total > r.total : l.dex > r.dex;
    });
    order_.clear();
    std::ostringstream o;
    o << "Initiative:";
    for (const auto& e : v) {
        order_.push_back(e.id);
        o << ' ' << actors_[static_cast<std::size_t>(e.id)]->Name() << '(' << e.total << ')';
    }
    Say(EventType::Info, -1, -1, o.str());
}

bool Combat::CheckEnd(Outcome& out) const {
    bool party = false, monsters = false;
    for (const auto& a : actors_) {
        if (!a->IsConscious()) continue;
        (a->team == Team::Party ? party : monsters) = true;
    }
    if (!monsters) { out = Outcome::Victory; return true; }
    if (!party) { out = Outcome::Defeat; return true; }
    return false;
}

CombatResult Combat::Finish(Outcome out, int rounds) {
    CombatResult r;
    r.outcome = out;
    r.rounds = rounds;
    int xp = 0, survivors = 0;
    for (const auto& a : actors_) {
        if (a->team == Team::Monsters && a->IsDead()) xp += a->sheet.xpValue;
        if (a->team == Team::Party && !a->IsDead()) ++survivors;
    }
    r.xpPerSurvivor = survivors ? xp / survivors : 0;
    const char* word = out == Outcome::Victory ? "Victory" : out == Outcome::Defeat ? "Defeat" : "The fight drags on without end";
    Say(EventType::CombatEnd, -1, -1, Cat(word, " after ", rounds, " round(s). XP per survivor: ", r.xpPerSurvivor), 0, 0, r.xpPerSurvivor);
    return r;
}

CombatResult Combat::Run(int maxRounds) {
    Say(EventType::CombatStart, -1, -1, "Roll for initiative!");
    RollInitiative();
    Outcome out = Outcome::Timeout;
    for (round_ = 1; round_ <= maxRounds; ++round_) {
        Say(EventType::RoundStart, -1, -1, Cat("Round ", round_), 0, 0, round_);
        for (int id : order_) {
            GameActor& a = *actors_[static_cast<std::size_t>(id)];
            if (a.IsDead()) continue;
            RunTurn(a);
            if (CheckEnd(out)) return Finish(out, round_);
        }
    }
    round_ = maxRounds;
    return Finish(Outcome::Timeout, maxRounds);
}

void Combat::DeathSave(GameActor& a) {
    const D20Result r = DiceRoller::D20();
    std::string msg;
    if (r.IsCrit()) {
        a.Heal(1);
        msg = Cat(a.Name(), " rolls a natural 20 on a death save and rises with 1 HP!");
    } else {
        if (r.IsFumble()) a.deathSaveFailures += 2;
        else if (r.natural >= 10) ++a.deathSaveSuccesses;
        else ++a.deathSaveFailures;
        msg = Cat(a.Name(), " death save [", r.natural, "]: ", a.deathSaveSuccesses, " success / ", a.deathSaveFailures, " fail");
        if (a.deathSaveFailures >= 3) {
            a.life = LifeState::Dead;
            RemoveFromGrid(a);
            Say(EventType::DeathSave, a.id, -1, msg, r.natural, 10);
            Say(EventType::Death, a.id, -1, Cat(a.Name(), " has died."));
            return;
        }
        if (a.deathSaveSuccesses >= 3) {
            a.life = LifeState::Stable;
            msg += Cat(" -> ", a.Name(), " is stable.");
        }
    }
    Say(EventType::DeathSave, a.id, -1, msg, r.natural, 10);
}

void Combat::RunTurn(GameActor& a) {
    a.sheet.economy.Reset(a.sheet.speedFt);
    a.dodging = false;
    a.sneakUsedThisTurn = false;
    a.dashedThisTurn = false;
    if (a.life == LifeState::Unconscious) { DeathSave(a); return; }
    if (a.life != LifeState::Alive) return;

    Say(EventType::TurnStart, a.id, -1, Cat(a.Name(), "'s turn"));
    int invalid = 0;
    for (int step = 0; step < 24; ++step) {
        if (!a.controller) break;
        const Action act = a.controller->DecideAction(TurnContext{*this, a});
        if (act.type == ActionType::EndTurn) break;
        const ActionResult r = Resolve(a, act);
        if (!r.ok) {
            Say(EventType::Info, a.id, -1, Cat("  (", a.Name(), ": ", r.reason, ")"));
            if (++invalid >= 3) break;
        }
        Outcome o;
        if (!a.IsConscious() || CheckEnd(o)) break;
    }
}

// ---------------------------------------------------------------- resolution

ActionResult Combat::Resolve(GameActor& a, const Action& act) {
    auto& eco = a.sheet.economy;
    switch (act.type) {
        case ActionType::Move: return DoMove(a, act.dest);
        case ActionType::Attack: return DoAttack(a, act);
        case ActionType::CastSpell: return DoCast(a, act);
        case ActionType::UsePotion: return DoPotion(a, act);
        case ActionType::Dash:
            if (!eco.action) return Fail("no action left");
            eco.action = false;
            eco.movementFt += a.sheet.speedFt;
            a.dashedThisTurn = true;
            Say(EventType::Info, a.id, -1, Cat(a.Name(), " dashes (", eco.movementFt, " ft of movement available)."));
            return {};
        case ActionType::Dodge:
            if (!eco.action) return Fail("no action left");
            eco.action = false;
            a.dodging = true;
            Say(EventType::Info, a.id, -1, Cat(a.Name(), " takes the Dodge action."));
            return {};
        case ActionType::SwapRole:
            if (!roleSwap_) return Fail("role swapping is not available");
            roleSwap_(act.role);
            return {};
        case ActionType::EndTurn: return {};
    }
    return Fail("unknown action");
}

void Combat::OpportunityAttacks(GameActor& mover, Pos from, Pos to) {
    for (const auto& e : actors_) {
        if (!mover.IsHostileTo(*e) || !e->IsConscious() || !e->sheet.economy.reaction || !mover.IsConscious()) continue;
        const WeaponAttack* melee = nullptr;
        for (const auto& w : e->sheet.attacks)
            if (!w.IsRanged()) { melee = &w; break; }
        if (!melee) continue;
        if (DistanceFt(from, e->pos) <= melee->rangeFt && DistanceFt(to, e->pos) > melee->rangeFt) {
            e->sheet.economy.reaction = false;
            AttackPreview pv = PreviewWeapon(*e, mover, *melee);
            if (pv.legal) ResolveWeaponAttack(*e, mover, *melee, pv, true);
        }
    }
}

ActionResult Combat::DoMove(GameActor& a, Pos dest) {
    auto& eco = a.sheet.economy;
    if (!grid_.InBounds(dest)) return Fail("destination is off the map");
    if (eco.movementFt < 5) return Fail("no movement left");
    if (dest == a.pos) return Fail("already there");
    if (!grid_.IsWalkableTerrain(dest) || grid_.OccupantAt(dest) >= 0) return Fail("destination is blocked");

    const auto pm = grid_.ComputePaths(a.pos, PassableFor(a));
    const auto path = pm.PathTo(dest);
    if (path.empty()) return Fail("no path to destination");

    // How many steps can we afford, backing off so we never stop inside another creature.
    std::size_t k = 0;
    int spent = 0;
    while (k < path.size() && spent + grid_.EnterCostFt(path[k]) <= eco.movementFt) spent += grid_.EnterCostFt(path[k++]);
    while (k > 0 && grid_.OccupantAt(path[k - 1]) >= 0) { --k; spent -= grid_.EnterCostFt(path[k]); }
    if (k == 0) return Fail("not enough movement");

    const Pos start = a.pos;
    Pos cur = a.pos;
    std::size_t done = 0;
    int cost = 0;
    for (; done < k; ++done) {
        OpportunityAttacks(a, cur, path[done]);
        if (!a.IsConscious()) break;
        cost += grid_.EnterCostFt(path[done]);
        cur = path[done];
    }
    // If interrupted while standing in a friend's square, fall back to the last free one.
    while (done > 0 && grid_.OccupantAt(cur) >= 0 && cur != start) { --done; cost -= grid_.EnterCostFt(path[done]); cur = done ? path[done - 1] : start; }

    eco.movementFt -= cost;
    if (!(cur == start)) {
        RemoveFromGrid(a);
        a.pos = cur;
        if (!a.IsDead()) grid_.SetOccupant(cur, a.id);
        Say(EventType::Move, a.id, -1, Cat(a.Name(), " moves ", cost, " ft to (", cur.x, ",", cur.y, ")."), 0, 0, cost);
    }
    return {};
}

Combat::AttackRollResult Combat::RollToHit(int toHit, const GameActor& t, const AttackPreview& pv) const {
    const D20Result r = DiceRoller::D20(pv.mode);
    const int ac = t.sheet.armorClass + pv.coverBonus;
    const int total = r.natural + toHit;
    bool crit = r.IsCrit();
    const bool hit = !r.IsFumble() && (crit || total >= ac);
    if (hit && pv.autoCritOnHit) crit = true;
    return {hit, crit, r.natural, total, ac};
}

void Combat::DealDamage(GameActor& src, GameActor& tgt, int amount, bool crit) {
    if (tgt.IsDead()) return;
    switch (tgt.ApplyDamage(amount, crit)) {
        case DamageResult::Dropped:
            Say(EventType::Down, tgt.id, src.id, Cat(tgt.Name(), " drops to 0 HP and falls unconscious!"));
            break;
        case DamageResult::DeathSaveFailed:
            Say(EventType::Info, tgt.id, src.id, Cat("  ", tgt.Name(), " suffers a death save failure (", tgt.deathSaveFailures, "/3)."));
            break;
        case DamageResult::Killed:
            RemoveFromGrid(tgt);
            Say(EventType::Death, tgt.id, src.id, Cat(tgt.Name(), " is slain!"));
            break;
        case DamageResult::Hurt: break;
    }
}

void Combat::ResolveWeaponAttack(GameActor& a, GameActor& t, const WeaponAttack& w, const AttackPreview& pv, bool opportunity) {
    const AttackRollResult r = RollToHit(w.toHit, t, pv);
    const std::string lead = Cat(a.Name(), opportunity ? " takes an opportunity attack on " : " attacks ", t.Name(), " with ", w.name, ": ");
    if (!r.hit) {
        Say(EventType::Attack, a.id, t.id, Cat(lead, "MISS [", r.total, " vs AC ", r.targetAc, "]", ModeTag(pv.mode)), r.total, r.targetAc);
        return;
    }
    int dmg = DiceRoller::Roll(w.damage, r.crit);
    int sneak = 0;
    if (pv.sneak && !a.sneakUsedThisTurn) {
        sneak = DiceRoller::Roll(DiceExpr{a.sheet.sneakAttackDice, 6, 0}, r.crit);
        a.sneakUsedThisTurn = true;
    }
    const int total = dmg + sneak;
    std::string line = Cat(lead, r.crit ? "CRIT" : "HIT", " [", r.total, " vs AC ", r.targetAc, "]", ModeTag(pv.mode), " for ", total, " ", ToString(w.type), " damage");
    if (sneak) line += Cat(" (incl. ", sneak, " sneak attack)");
    Say(EventType::Attack, a.id, t.id, line + "!", r.total, r.targetAc, total);
    DealDamage(a, t, total, r.crit);
}

ActionResult Combat::DoAttack(GameActor& a, const Action& act) {
    auto& eco = a.sheet.economy;
    if (!eco.action) return Fail("no action left");
    GameActor* t = Find(act.targetId);
    if (!t || t == &a) return Fail("invalid target");
    const auto& atks = a.sheet.attacks;
    if (atks.empty()) return Fail("has no attacks");

    int idx = act.attackIndex;
    if (idx < 0) {
        std::string why = "target is out of reach";
        for (int i = 0; i < static_cast<int>(atks.size()); ++i) {
            const AttackPreview pv = PreviewWeapon(a, *t, atks[static_cast<std::size_t>(i)]);
            if (pv.legal) { idx = i; break; }
            why = pv.whyNot;
        }
        if (idx < 0) return Fail(why);
    }
    if (idx >= static_cast<int>(atks.size())) return Fail("no such attack");
    const WeaponAttack& w = atks[static_cast<std::size_t>(idx)];
    const AttackPreview pv = PreviewWeapon(a, *t, w);
    if (!pv.legal) return Fail(pv.whyNot);
    eco.action = false;
    ResolveWeaponAttack(a, *t, w, pv, false);
    return {};
}

bool Combat::RollSave(const GameActor& t, Ability ab, int dc, int& total) const {
    const int nat = DiceRoller::D20().natural;
    total = nat + t.sheet.SaveBonus(ab);
    if (t.IsDown() && (ab == Ability::Str || ab == Ability::Dex)) return false;  // helpless
    return total >= dc;
}

ActionResult Combat::DoCast(GameActor& a, const Action& act) {
    const Spell* sp = Content::FindSpell(act.spellId);
    if (!sp) return Fail("unknown spell");
    if (!a.sheet.KnowsSpell(sp->id)) return Fail(Cat(a.Name(), " does not know ", sp->name));
    auto& eco = a.sheet.economy;
    if (sp->bonusAction ? !eco.bonusAction : !eco.action) return Fail("no action available for that spell");

    int slot = 0;
    if (sp->level > 0) {
        slot = act.slotLevel > 0 ? act.slotLevel : a.sheet.LowestSlotAtLeast(sp->level);
        if (slot < sp->level || slot > 9 || a.sheet.slots[static_cast<std::size_t>(slot)] <= 0) return Fail("no spell slot available");
    }
    GameActor* t = Find(act.targetId);
    if (!t || t->IsDead()) return Fail("invalid target");
    const bool heal = sp->kind == SpellKind::Heal;
    if (heal && t->team != a.team) return Fail("can only heal allies");
    if (!heal && sp->radiusFt == 0 && t->team == a.team) return Fail("cannot target an ally with that spell");
    if (DistanceFt(a.pos, t->pos) > sp->rangeFt) return Fail("target out of range");
    if (!grid_.HasLineOfSight(a.pos, t->pos)) return Fail("no line of sight");

    AttackPreview pv;
    if (sp->kind == SpellKind::AttackRoll) {
        pv = PreviewAttack(a, *t, true, sp->rangeFt, sp->rangeFt, false);
        if (!pv.legal) return Fail(pv.whyNot);
    }

    (sp->bonusAction ? eco.bonusAction : eco.action) = false;
    if (slot > 0) --a.sheet.slots[static_cast<std::size_t>(slot)];

    const std::string lead = Cat(a.Name(), " casts ", sp->name, slot ? Cat(" (slot ", slot, ")") : std::string(), " on ", t->Name(), ": ");
    const DiceExpr dice = SpellDice(a.sheet, *sp, slot);
    const int dc = a.sheet.SpellSaveDc();

    switch (sp->kind) {
        case SpellKind::AttackRoll: {
            const AttackRollResult r = RollToHit(a.sheet.SpellAttackBonus(), *t, pv);
            if (!r.hit) {
                Say(EventType::Spell, a.id, t->id, Cat(lead, "MISS [", r.total, " vs AC ", r.targetAc, "]", ModeTag(pv.mode)), r.total, r.targetAc);
                break;
            }
            const int dmg = DiceRoller::Roll(dice, r.crit);
            Say(EventType::Spell, a.id, t->id, Cat(lead, r.crit ? "CRIT" : "HIT", " [", r.total, " vs AC ", r.targetAc, "]", ModeTag(pv.mode), " for ", dmg, " ", ToString(sp->damageType), " damage!"), r.total, r.targetAc, dmg);
            DealDamage(a, *t, dmg, r.crit);
            break;
        }
        case SpellKind::AutoHit: {
            int dmg = 0;
            const int n = SpellProjectiles(*sp, slot);
            for (int i = 0; i < n; ++i) dmg += DiceRoller::Roll(dice);
            Say(EventType::Spell, a.id, t->id, Cat(lead, n, " darts strike for ", dmg, " ", ToString(sp->damageType), " damage!"), 0, 0, dmg);
            DealDamage(a, *t, dmg, false);
            break;
        }
        case SpellKind::SaveNegates: {
            int total = 0;
            if (RollSave(*t, sp->save, dc, total)) {
                Say(EventType::Spell, a.id, t->id, Cat(lead, t->Name(), " resists (", ToString(sp->save), " save ", total, " vs DC ", dc, ")."), total, dc);
            } else {
                const int dmg = DiceRoller::Roll(dice);
                Say(EventType::Spell, a.id, t->id, Cat(lead, "FAILED ", ToString(sp->save), " save (", total, " vs DC ", dc, ") for ", dmg, " ", ToString(sp->damageType), " damage!"), total, dc, dmg);
                DealDamage(a, *t, dmg, false);
            }
            break;
        }
        case SpellKind::SaveForHalf: {
            const int dmg = DiceRoller::Roll(dice);  // rolled once for the whole burst
            Say(EventType::Spell, a.id, t->id, Cat(lead, "a burst of ", ToString(sp->damageType), " (", dmg, " damage, DC ", dc, " ", ToString(sp->save), " for half)."), 0, dc, dmg);
            std::vector<GameActor*> hit;
            for (const auto& o : actors_)
                if (!o->IsDead() && WithinBurst(t->pos, o->pos, sp->radiusFt) && grid_.HasLineOfSight(t->pos, o->pos)) hit.push_back(o.get());
            for (GameActor* o : hit) {
                int total = 0;
                const bool saved = RollSave(*o, sp->save, dc, total);
                const int applied = saved ? dmg / 2 : dmg;
                Say(EventType::Spell, a.id, o->id, Cat("   ", o->Name(), saved ? " saves" : " fails", " (", total, ") and takes ", applied, " damage."), total, dc, applied);
                DealDamage(a, *o, applied, false);
            }
            break;
        }
        case SpellKind::Heal: {
            const int roll = DiceRoller::Roll(dice) + (sp->addsCastingMod ? std::max(0, a.sheet.Mod(a.sheet.spellAbility)) : 0);
            const bool wasDown = t->IsDown();
            const int healed = t->Heal(roll);
            Say(EventType::Heal, a.id, t->id, Cat(lead, "restores ", healed, " HP", wasDown ? Cat(" and ", t->Name(), " rises!") : std::string(".")), 0, 0, healed);
            break;
        }
    }
    return {};
}

ActionResult Combat::DoPotion(GameActor& a, const Action& act) {
    auto& eco = a.sheet.economy;
    if (!eco.action) return Fail("no action left");
    if (a.sheet.potions <= 0) return Fail("no potions left");
    GameActor* t = act.targetId >= 0 ? Find(act.targetId) : &a;
    if (!t || t->IsDead() || t->team != a.team) return Fail("invalid potion target");
    if (DistanceFt(a.pos, t->pos) > 5) return Fail("target is too far away for a potion");
    --a.sheet.potions;
    eco.action = false;
    const bool wasDown = t->IsDown();
    const int healed = t->Heal(DiceRoller::Roll(kPotionOfHealing));
    Say(EventType::Heal, a.id, t->id, Cat(a.Name(), " uses a potion of healing on ", t == &a ? std::string("themself") : t->Name(), ": +", healed, " HP", wasDown ? " and they rise!" : "."), 0, 0, healed);
    return {};
}

}  // namespace dnd
