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

template <class T>
bool Contains(const std::vector<T>& v, const T& x) { return std::find(v.begin(), v.end(), x) != v.end(); }

int CoverBonusFor(Cover c) { return c == Cover::Half ? 2 : c == Cover::ThreeQuarters ? 5 : 0; }

std::string Note(int raw, int applied) {
    return raw == applied ? std::string() : Cat(" (", applied < raw ? "resisted" : "vulnerable", ": ", applied, ")");
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

void Combat::ClearActors() {
    for (const auto& a : actors_) RemoveFromGrid(*a);
    actors_.clear();
    order_.clear();
}

GameActor* Combat::Find(int id) {
    return id >= 0 && id < static_cast<int>(actors_.size()) ? actors_[static_cast<std::size_t>(id)].get() : nullptr;
}
const GameActor* Combat::Find(int id) const {
    return id >= 0 && id < static_cast<int>(actors_.size()) ? actors_[static_cast<std::size_t>(id)].get() : nullptr;
}

void Combat::Say(EventType t, int actor, int target, std::string text, int roll, int vs, int amount) const {
    bus_.Emit(Event{t, actor, target, roll, vs, amount, std::move(text), 0, {}});
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
    p.coverBonus = CoverBonusFor(p.cover);

    bool adv = false, dis = false;
    // attacker's state
    if (a.HasCondition(Condition::Poisoned) || a.HasCondition(Condition::Frightened) || a.HasCondition(Condition::Restrained) ||
        a.HasCondition(Condition::Blinded) || a.HasCondition(Condition::Prone) || a.sheet.exhaustion >= 3)
        dis = true;
    // target's state
    if (t.dodging && t.IsConscious()) dis = true;
    if (t.IsDown()) {
        adv = true;
        p.autoCritOnHit = p.distanceFt <= 5;
    }
    if (t.HasCondition(Condition::Blinded) || t.HasCondition(Condition::Restrained) || t.HasCondition(Condition::Stunned)) adv = true;
    if (t.HasCondition(Condition::Paralyzed)) {
        adv = true;
        if (p.distanceFt <= 5) p.autoCritOnHit = true;
    }
    if (t.HasCondition(Condition::Prone)) (p.distanceFt <= 5 ? adv : dis) = true;
    for (const auto& e : t.effects)
        if (e.advantageAgainst) adv = true;
    if (ranged) {
        if (p.distanceFt > rangeFt) dis = true;
        for (const auto& o : actors_)
            if (a.IsHostileTo(*o) && o->IsConscious() && !o->IsIncapacitated() && DistanceFt(a.pos, o->pos) <= 5) { dis = true; break; }
    }
    // an ally next to the target (pack tactics, sneak attack)
    bool allyAdjacent = false, allyAdjacentActive = false;
    for (const auto& o : actors_)
        if (o->id != a.id && o->team == a.team && o->IsConscious() && DistanceFt(o->pos, t.pos) <= 5) {
            allyAdjacent = true;
            if (!o->IsIncapacitated()) allyAdjacentActive = true;
        }
    if (a.sheet.Trait("pack_tactics") && allyAdjacentActive) adv = true;
    p.mode = CombineModes(adv, dis);

    if (sneakCapable && a.sheet.sneakAttackDice > 0 && !a.sneakUsedThisTurn && p.mode != RollMode::Disadvantage)
        p.sneak = p.mode == RollMode::Advantage || allyAdjacent;
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

// SRD: resistance/vulnerability apply after other modifiers; duplicates count once; halving rounds down.
int Combat::AdjustDamage(const GameActor& t, int amount, DamageType type, bool magical) {
    if (Contains(t.sheet.immune, type)) return 0;
    bool resist = Contains(t.sheet.resist, type);
    if (IsPhysical(type)) {
        if (t.sheet.resistNonmagicalPhysical && !magical) resist = true;
        for (const auto& e : t.effects)
            if (e.resistPhysical) resist = true;
    }
    const bool vuln = Contains(t.sheet.vulnerable, type);
    if (resist && !vuln) return amount / 2;
    if (vuln && !resist) return amount * 2;
    return amount;
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
          << "  AC " << a->EffectiveAc() << "  @(" << a->pos.x << "," << a->pos.y << ")" << (a->IsDown() ? "  DOWN" : "");
        for (const auto& e : a->effects) o << "  <" << e.name << ">";
        o << '\n';
    }
    return o.str();
}

// ---------------------------------------------------------------- turn machinery

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

void Combat::Finish(Outcome out) {
    finished_ = true;
    awaiting_ = -1;
    result_.outcome = out;
    result_.rounds = round_;
    int xp = 0, survivors = 0;
    for (const auto& a : actors_) {
        if (a->team == Team::Monsters && a->IsDead()) xp += a->sheet.xpValue;
        if (a->team == Team::Party && !a->IsDead()) ++survivors;
    }
    result_.xpTotal = xp;
    result_.xpPerSurvivor = survivors ? xp / survivors : 0;
    const char* word = out == Outcome::Victory ? "Victory" : out == Outcome::Defeat ? "Defeat" : "The fight drags on without end";
    Say(EventType::CombatEnd, -1, -1, Cat(word, " after ", round_, " round(s). XP per survivor: ", result_.xpPerSurvivor), 0, 0, result_.xpPerSurvivor);
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

void Combat::BeginTurn(GameActor& a) {
    // Effects whose source's turn is starting (guiding bolt's glow) run out now.
    for (const auto& other : actors_)
        for (std::size_t i = other->effects.size(); i-- > 0;)
            if (other->effects[i].expireAtSourceTurn && other->effects[i].sourceId == a.id) RemoveEffect(*other, i, "fades");
    a.sheet.economy.Reset(a.SpeedFt());
    a.dodging = false;
    a.sneakUsedThisTurn = false;
    a.dashedThisTurn = false;
    if (a.IsConscious()) {
        if (a.IsIncapacitated()) {
            a.sheet.economy.action = a.sheet.economy.bonusAction = a.sheet.economy.reaction = false;
        }
        if (const int regen = a.sheet.Trait("regeneration"); regen > 0 && a.sheet.hp > 0 && a.sheet.hp < a.sheet.maxHp) {
            const int healed = a.Heal(regen);
            if (healed) Say(EventType::Heal, a.id, a.id, Cat(a.Name(), " regenerates ", healed, " HP."), 0, 0, healed);
        }
    }
}

void Combat::FinishTurn(GameActor& a) {
    if (a.IsConscious()) TickEndOfTurn(a);
    inTurn_ = false;
    ++idx_;
}

ActionResult Combat::StepAction(GameActor& a, const Action& act, bool countInvalid) {
    if (act.type == ActionType::EndTurn || (countInvalid && ++steps_ > 24)) {
        FinishTurn(a);
        return {};
    }
    const ActionResult r = Resolve(a, act);
    if (a.controller) a.controller->OnActionResult(TurnContext{*this, a}, act, r.ok, r.reason);
    if (!r.ok) {
        Say(EventType::Info, a.id, -1, Cat("  (", a.Name(), ": ", r.reason, ")"));
        if (countInvalid && ++invalid_ >= 3 && inTurn_) FinishTurn(a);
    }
    if (inTurn_ && !a.IsConscious()) FinishTurn(a);
    return r;
}

StepStatus Combat::Advance() {
    if (finished_) return StepStatus::Finished;
    if (!started_) {
        started_ = true;
        Say(EventType::CombatStart, -1, -1, "Roll for initiative!");
        RollInitiative();
        idx_ = order_.size();  // forces round 1 to begin
        round_ = 0;
    }
    for (;;) {
        Outcome out;
        if (CheckEnd(out)) { Finish(out); return StepStatus::Finished; }

        if (!inTurn_) {
            if (idx_ >= order_.size()) {
                if (round_ >= maxRounds_) { Finish(Outcome::Timeout); return StepStatus::Finished; }
                ++round_;
                idx_ = 0;
                Say(EventType::RoundStart, -1, -1, Cat("Round ", round_), 0, 0, round_);
            }
            GameActor& a = *actors_[static_cast<std::size_t>(order_[idx_])];
            if (a.IsDead()) { ++idx_; continue; }
            BeginTurn(a);
            if (a.life == LifeState::Unconscious) { DeathSave(a); ++idx_; continue; }
            if (a.life != LifeState::Alive) { ++idx_; continue; }
            Say(EventType::TurnStart, a.id, -1, Cat(a.Name(), "'s turn"));
            inTurn_ = true;
            invalid_ = steps_ = 0;
            if (a.IsIncapacitated()) {
                Say(EventType::Info, a.id, -1, Cat("  ", a.Name(), " is incapacitated and cannot act."));
                FinishTurn(a);
                continue;
            }
        }
        GameActor& a = *actors_[static_cast<std::size_t>(order_[idx_])];
        const Action act = a.controller ? a.controller->DecideAction(TurnContext{*this, a}) : Action::End();
        if (act.type == ActionType::AwaitInput) {
            awaiting_ = a.id;
            return StepStatus::AwaitingInput;
        }
        awaiting_ = -1;
        StepAction(a, act, true);
    }
}

ActionResult Combat::Submit(int actorId, const Action& action) {
    if (finished_) return Fail("the fight is over");
    if (awaiting_ < 0 || awaiting_ != actorId) return Fail(awaiting_ < 0 ? "no input is being awaited" : Cat("waiting for actor ", awaiting_));
    GameActor& a = *actors_[static_cast<std::size_t>(actorId)];
    if (action.type == ActionType::AwaitInput) return Fail("invalid action");
    awaiting_ = -1;
    const ActionResult r = StepAction(a, action, false);
    if (!r.ok && inTurn_) awaiting_ = actorId;  // let the player try again
    return r;
}

CombatResult Combat::Run(int maxRounds) {
    maxRounds_ = maxRounds;
    while (Advance() == StepStatus::AwaitingInput) {
        // A blocking caller has no way to answer a remote prompt.
        Finish(Outcome::Timeout);
        break;
    }
    return result_;
}

// ---------------------------------------------------------------- effects

void Combat::ApplyEffect(GameActor& target, Effect e) {
    if (e.condition != Condition::None && Contains(target.sheet.conditionImmune, e.condition)) {
        Say(EventType::Info, e.sourceId, target.id, Cat("   ", target.Name(), " is immune to being ", ToString(e.condition), "."));
        return;
    }
    for (std::size_t i = 0; i < target.effects.size(); ++i)
        if (target.effects[i].id == e.id && target.effects[i].sourceId == e.sourceId) { target.effects.erase(target.effects.begin() + static_cast<long>(i)); break; }
    const bool disabling = e.condition == Condition::Paralyzed || e.condition == Condition::Stunned || e.condition == Condition::Incapacitated;
    const std::string label = e.condition != Condition::None ? std::string(ToString(e.condition)) : e.name;
    const int source = e.sourceId;
    target.effects.push_back(std::move(e));
    Say(EventType::Info, source, target.id, Cat("   ", target.Name(), " is now ", label, "."));
    if (disabling && !target.concentrating.empty()) EndConcentration(target);
}

void Combat::RemoveEffect(GameActor& target, std::size_t index, const char* why) {
    const Effect e = target.effects[index];
    target.effects.erase(target.effects.begin() + static_cast<long>(index));
    Say(EventType::Info, e.sourceId, target.id, Cat("   ", e.name, " on ", target.Name(), " ", why, "."));
    if (e.concentration) {  // last effect of that concentration spell? then the caster is free again
        bool any = false;
        for (const auto& o : actors_)
            for (const auto& x : o->effects)
                if (x.concentration && x.sourceId == e.sourceId) any = true;
        if (!any)
            if (GameActor* caster = Find(e.sourceId)) caster->concentrating.clear();
    }
}

void Combat::EndConcentration(GameActor& caster) {
    if (caster.concentrating.empty()) return;
    const Spell* sp = Content::FindSpell(caster.concentrating);
    Say(EventType::Info, caster.id, -1, Cat("   ", caster.Name(), " loses concentration on ", sp ? sp->name : caster.concentrating, "."));
    caster.concentrating.clear();
    for (const auto& o : actors_)
        for (std::size_t i = o->effects.size(); i-- > 0;)
            if (o->effects[i].concentration && o->effects[i].sourceId == caster.id) o->effects.erase(o->effects.begin() + static_cast<long>(i));
}

void Combat::TickEndOfTurn(GameActor& a) {
    for (std::size_t i = a.effects.size(); i-- > 0;) {
        Effect& e = a.effects[i];
        if (e.saveEnds) {
            int total = 0;
            if (RollSave(a, e.saveAbility, e.saveDc, total)) {
                Say(EventType::Info, a.id, -1, Cat("   ", a.Name(), " shakes off ", e.name, " (", ToString(e.saveAbility), " save ", total, " vs DC ", e.saveDc, ")."));
                RemoveEffect(a, i, "ends");
                continue;
            }
        }
        if (e.rounds > 0 && --e.rounds == 0) RemoveEffect(a, i, "expires");
    }
}

void Combat::ConsumeAdvantageEffects(GameActor& target) {
    for (std::size_t i = target.effects.size(); i-- > 0;)
        if (target.effects[i].advantageAgainst) RemoveEffect(target, i, "is used up");
}

// ---------------------------------------------------------------- resolution

ActionResult Combat::Resolve(GameActor& a, const Action& act) {
    auto& eco = a.sheet.economy;
    switch (act.type) {
        case ActionType::Move: return DoMove(a, act.dest);
        case ActionType::Attack: return DoAttack(a, act);
        case ActionType::CastSpell: return DoCast(a, act);
        case ActionType::UsePotion: return DoPotion(a, act);
        case ActionType::UseFeature: return DoFeature(a, act);
        case ActionType::Dash:
            if (!eco.action) return Fail("no action left");
            eco.action = false;
            eco.movementFt += a.SpeedFt();
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
        case ActionType::EndTurn:
        case ActionType::AwaitInput: return {};
    }
    return Fail("unknown action");
}

void Combat::OpportunityAttacks(GameActor& mover, Pos from, Pos to) {
    for (const auto& e : actors_) {
        if (!mover.IsHostileTo(*e) || !e->IsConscious() || e->IsIncapacitated() || !e->sheet.economy.reaction || !mover.IsConscious()) continue;
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

    if (a.HasCondition(Condition::Prone)) {  // SRD: standing up costs half your speed
        const int cost = a.SpeedFt() / 2;
        if (eco.movementFt < cost || a.SpeedFt() == 0) return Fail("not enough movement to stand up");
        eco.movementFt -= cost;
        for (std::size_t i = a.effects.size(); i-- > 0;)
            if (a.effects[i].condition == Condition::Prone) a.effects.erase(a.effects.begin() + static_cast<long>(i));
        Say(EventType::Info, a.id, -1, Cat(a.Name(), " stands up (", cost, " ft)."));
        if (eco.movementFt < 5) return {};
    }

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

Combat::AttackRollResult Combat::RollToHit(const GameActor& a, int toHit, const GameActor& t, const AttackPreview& pv) {
    const D20Result r = DiceRoller::D20(pv.mode);
    const int ac = t.EffectiveAc() + pv.coverBonus;
    const int total = r.natural + toHit;
    bool crit = r.IsCrit();
    const bool hit = !r.IsFumble() && (crit || total >= ac);
    if (hit && pv.autoCritOnHit) crit = true;
    (void)a;
    return {hit, crit, r.natural, total, ac};
}

void Combat::DealDamage(GameActor& src, GameActor& tgt, int amount, bool crit) {
    if (tgt.IsDead()) return;
    const DamageResult res = tgt.ApplyDamage(amount, crit);
    switch (res) {
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
    // SRD: damage forces a CON save (DC 10 or half the damage) to keep concentration; 0 HP ends it outright.
    if (!tgt.concentrating.empty() && amount > 0) {
        if (res == DamageResult::Dropped || res == DamageResult::Killed) {
            EndConcentration(tgt);
        } else {
            const int dc = std::max(10, amount / 2);
            int total = 0;
            if (RollSave(tgt, Ability::Con, dc, total)) Say(EventType::Info, tgt.id, -1, Cat("   ", tgt.Name(), " holds concentration (CON ", total, " vs DC ", dc, ")."));
            else EndConcentration(tgt);
        }
    }
}

void Combat::ResolveWeaponAttack(GameActor& a, GameActor& t, const WeaponAttack& w, const AttackPreview& pv, bool opportunity) {
    const AttackRollResult r = RollToHit(a, w.toHit, t, pv);
    ConsumeAdvantageEffects(t);
    const std::string lead = Cat(a.Name(), opportunity ? " takes an opportunity attack on " : " attacks ", t.Name(), " with ", w.name, ": ");
    if (!r.hit) {
        Say(EventType::Attack, a.id, t.id, Cat(lead, "MISS [", r.total, " vs AC ", r.targetAc, "]", ModeTag(pv.mode)), r.total, r.targetAc);
        return;
    }
    int dmg = DiceRoller::Roll(w.damage, r.crit);
    if (!w.IsRanged())
        for (const auto& e : a.effects) dmg += e.meleeDamageBonus;  // rage
    int sneak = 0;
    if (pv.sneak && !a.sneakUsedThisTurn) {
        sneak = DiceRoller::Roll(DiceExpr{a.sheet.sneakAttackDice, 6, 0}, r.crit);
        a.sneakUsedThisTurn = true;
    }
    const int raw = dmg + sneak;
    const int total = AdjustDamage(t, raw, w.type, w.magical);
    std::string line = Cat(lead, r.crit ? "CRIT" : "HIT", " [", r.total, " vs AC ", r.targetAc, "]", ModeTag(pv.mode), " for ", total, " ", ToString(w.type), " damage");
    if (sneak) line += Cat(" (incl. ", sneak, " sneak attack)");
    line += Note(raw, total);
    Say(EventType::Attack, a.id, t.id, line + "!", r.total, r.targetAc, total);
    DealDamage(a, t, total, r.crit);

    if (w.rider.present && !t.IsDead()) {
        bool exempt = false;
        for (const auto& tag : w.rider.exceptTags) exempt = exempt || t.sheet.HasTag(tag);
        if (!exempt) {
            int stotal = 0;
            if (!RollSave(t, w.rider.save, w.rider.dc, stotal)) {
                Effect e;
                e.id = w.name + "_rider";
                e.name = Cat(w.name, " (", ToString(w.rider.condition), ")");
                e.condition = w.rider.condition;
                e.rounds = w.rider.rounds;
                e.sourceId = a.id;
                e.saveEnds = w.rider.saveEnds;
                e.saveAbility = w.rider.save;
                e.saveDc = w.rider.dc;
                Say(EventType::Info, a.id, t.id, Cat("   ", t.Name(), " fails a ", ToString(w.rider.save), " save (", stotal, " vs DC ", w.rider.dc, ")."));
                ApplyEffect(t, std::move(e));
            } else {
                Say(EventType::Info, a.id, t.id, Cat("   ", t.Name(), " resists the effect (", ToString(w.rider.save), " ", stotal, " vs DC ", w.rider.dc, ")."));
            }
        }
    }
}

ActionResult Combat::DoAttack(GameActor& a, const Action& act) {
    auto& eco = a.sheet.economy;
    const bool starting = eco.attacksLeft <= 0;
    if (starting && !eco.action) return Fail("no action left");
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
    if (starting) {  // the Attack action: one strike now, the rest are Extra Attack
        eco.action = false;
        eco.attacksLeft = std::max(1, a.sheet.attacksPerAction);
    }
    --eco.attacksLeft;
    ResolveWeaponAttack(a, *t, w, pv, false);
    return {};
}

bool Combat::RollSave(const GameActor& t, Ability ab, int dc, int& total, int coverBonus) const {
    const bool helpless = t.IsDown() || t.HasCondition(Condition::Paralyzed) || t.HasCondition(Condition::Stunned);
    RollMode mode = RollMode::Normal;
    if (t.sheet.exhaustion >= 3 || (ab == Ability::Dex && t.HasCondition(Condition::Restrained))) mode = RollMode::Disadvantage;
    const int nat = DiceRoller::D20(mode).natural;
    total = nat + t.sheet.SaveBonus(ab) + (ab == Ability::Dex ? coverBonus : 0);
    if (helpless && (ab == Ability::Str || ab == Ability::Dex)) return false;
    return total >= dc;
}

ActionResult Combat::DoCast(GameActor& a, const Action& act) {
    const Spell* sp = Content::FindSpell(act.spellId);
    if (!sp) return Fail("unknown spell");
    if (!a.sheet.KnowsSpell(sp->id)) return Fail(Cat(a.Name(), " does not know ", sp->name));
    auto& eco = a.sheet.economy;
    if (eco.attacksLeft > 0) return Fail("already used the Attack action this turn");
    if (sp->bonusAction ? !eco.bonusAction : !eco.action) return Fail("no action available for that spell");

    int slot = 0;
    if (sp->level > 0) {
        slot = act.slotLevel > 0 ? act.slotLevel : a.sheet.LowestSlotAtLeast(sp->level);
        if (slot < sp->level || slot > 9 || a.sheet.slots[static_cast<std::size_t>(slot)] <= 0) return Fail("no spell slot available");
    }
    GameActor* t = Find(act.targetId);
    if (!t || t->IsDead()) return Fail("invalid target");
    const bool friendly = sp->kind == SpellKind::Heal || sp->kind == SpellKind::Buff;
    if (friendly && t->team != a.team) return Fail("that spell can only target allies");
    if (!friendly && sp->radiusFt == 0 && t->team == a.team) return Fail("cannot target an ally with that spell");
    if (!sp->requiresTag.empty() && !t->sheet.HasTag(sp->requiresTag)) return Fail(Cat(sp->name, " only affects a ", sp->requiresTag));
    if (DistanceFt(a.pos, t->pos) > sp->rangeFt) return Fail("target out of range");
    if (!grid_.HasLineOfSight(a.pos, t->pos)) return Fail("no line of sight");

    AttackPreview pv;
    if (sp->kind == SpellKind::AttackRoll) {
        pv = PreviewAttack(a, *t, true, sp->rangeFt, sp->rangeFt, false);
        if (!pv.legal) return Fail(pv.whyNot);
    }

    (sp->bonusAction ? eco.bonusAction : eco.action) = false;
    if (slot > 0) --a.sheet.slots[static_cast<std::size_t>(slot)];
    if (sp->concentration) {  // only one concentration spell at a time
        EndConcentration(a);
        a.concentrating = sp->id;
    }

    const std::string lead = Cat(a.Name(), " casts ", sp->name, slot ? Cat(" (slot ", slot, ")") : std::string(), " on ", t->Name(), ": ");
    const DiceExpr dice = SpellDice(a.sheet, *sp, slot);
    const int dc = a.sheet.SpellSaveDc();

    auto makeEffect = [&](const Spell& s) {
        Effect e;
        e.id = s.id;
        e.name = s.name;
        e.condition = s.condition;
        e.rounds = s.durationRounds;
        e.sourceId = a.id;
        e.concentration = s.concentration;
        e.saveEnds = s.saveEnds;
        e.saveAbility = s.save;
        e.saveDc = a.sheet.SpellSaveDc();
        e.acBonus = s.acBonus;
        return e;
    };

    switch (sp->kind) {
        case SpellKind::AttackRoll: {
            const AttackRollResult r = RollToHit(a, a.sheet.SpellAttackBonus(), *t, pv);
            ConsumeAdvantageEffects(*t);
            if (!r.hit) {
                Say(EventType::Spell, a.id, t->id, Cat(lead, "MISS [", r.total, " vs AC ", r.targetAc, "]", ModeTag(pv.mode)), r.total, r.targetAc);
                break;
            }
            const int raw = DiceRoller::Roll(dice, r.crit);
            const int dmg = AdjustDamage(*t, raw, sp->damageType, true);
            Say(EventType::Spell, a.id, t->id, Cat(lead, r.crit ? "CRIT" : "HIT", " [", r.total, " vs AC ", r.targetAc, "]", ModeTag(pv.mode), " for ", dmg, " ", ToString(sp->damageType), " damage", Note(raw, dmg), "!"), r.total, r.targetAc, dmg);
            DealDamage(a, *t, dmg, r.crit);
            if (sp->grantsAdvantage && !t->IsDead()) {
                Effect e = makeEffect(*sp);
                e.name = "outlined in light";
                e.advantageAgainst = true;
                e.expireAtSourceTurn = true;
                e.rounds = -1;
                e.concentration = false;
                ApplyEffect(*t, std::move(e));
            }
            break;
        }
        case SpellKind::AutoHit: {
            int raw = 0;
            const int n = SpellProjectiles(*sp, slot);
            for (int i = 0; i < n; ++i) raw += DiceRoller::Roll(dice);
            const int dmg = AdjustDamage(*t, raw, sp->damageType, true);
            Say(EventType::Spell, a.id, t->id, Cat(lead, n, " darts strike for ", dmg, " ", ToString(sp->damageType), " damage", Note(raw, dmg), "!"), 0, 0, dmg);
            DealDamage(a, *t, dmg, false);
            break;
        }
        case SpellKind::SaveNegates: {
            int total = 0;
            const int cover = CoverBonusFor(grid_.CoverBetween(a.pos, t->pos));
            if (RollSave(*t, sp->save, dc, total, cover)) {
                Say(EventType::Spell, a.id, t->id, Cat(lead, t->Name(), " resists (", ToString(sp->save), " save ", total, " vs DC ", dc, ")."), total, dc);
            } else {
                const int raw = DiceRoller::Roll(dice);
                const int dmg = AdjustDamage(*t, raw, sp->damageType, true);
                Say(EventType::Spell, a.id, t->id, Cat(lead, "FAILED ", ToString(sp->save), " save (", total, " vs DC ", dc, ") for ", dmg, " ", ToString(sp->damageType), " damage", Note(raw, dmg), "!"), total, dc, dmg);
                DealDamage(a, *t, dmg, false);
            }
            break;
        }
        case SpellKind::SaveForHalf: {
            const int raw = DiceRoller::Roll(dice);  // rolled once for the whole burst
            Say(EventType::Spell, a.id, t->id, Cat(lead, "a burst of ", ToString(sp->damageType), " (", raw, " damage, DC ", dc, " ", ToString(sp->save), " for half)."), 0, dc, raw);
            std::vector<GameActor*> hit;
            for (const auto& o : actors_)
                if (!o->IsDead() && WithinBurst(t->pos, o->pos, sp->radiusFt) && grid_.HasLineOfSight(t->pos, o->pos)) hit.push_back(o.get());
            for (GameActor* o : hit) {
                int total = 0;
                const int cover = CoverBonusFor(grid_.CoverBetween(t->pos, o->pos));
                const bool saved = RollSave(*o, sp->save, dc, total, cover);
                const int applied = AdjustDamage(*o, saved ? raw / 2 : raw, sp->damageType, true);
                Say(EventType::Spell, a.id, o->id, Cat("   ", o->Name(), saved ? " saves" : " fails", " (", total, ") and takes ", applied, " damage."), total, dc, applied);
                DealDamage(a, *o, applied, false);
            }
            break;
        }
        case SpellKind::Condition: {
            int total = 0;
            const int cover = CoverBonusFor(grid_.CoverBetween(a.pos, t->pos));
            if (RollSave(*t, sp->save, dc, total, cover)) {
                Say(EventType::Spell, a.id, t->id, Cat(lead, t->Name(), " resists (", ToString(sp->save), " save ", total, " vs DC ", dc, ")."), total, dc);
            } else {
                Say(EventType::Spell, a.id, t->id, Cat(lead, "FAILED ", ToString(sp->save), " save (", total, " vs DC ", dc, ")!"), total, dc);
                ApplyEffect(*t, makeEffect(*sp));
            }
            break;
        }
        case SpellKind::Buff: {
            Say(EventType::Spell, a.id, t->id, Cat(lead, "+", sp->acBonus, " AC."));
            ApplyEffect(*t, makeEffect(*sp));
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
    if (!eco.action || eco.attacksLeft > 0) return Fail("no action left");
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

ActionResult Combat::DoFeature(GameActor& a, const Action& act) {
    auto& eco = a.sheet.economy;
    const std::string& f = act.featureId;
    if (a.sheet.FeatureUses(f) <= 0) return Fail(Cat("no uses of ", f, " left"));
    if (!eco.bonusAction) return Fail("no bonus action left");
    if (f == "rage") {
        for (const auto& e : a.effects)
            if (e.id == "rage") return Fail("already raging");
        Effect e;
        e.id = "rage";
        e.name = "Rage";
        e.rounds = 10;
        e.sourceId = a.id;
        e.resistPhysical = true;
        e.meleeDamageBonus = 2 + (a.sheet.level >= 9 ? 1 : 0) + (a.sheet.level >= 16 ? 1 : 0);
        --a.sheet.featureUses[f];
        eco.bonusAction = false;
        Say(EventType::Info, a.id, -1, Cat(a.Name(), " flies into a RAGE! (+", e.meleeDamageBonus, " melee damage, resists weapon damage)"));
        a.effects.push_back(std::move(e));
        return {};
    }
    if (f == "second_wind") {
        --a.sheet.featureUses[f];
        eco.bonusAction = false;
        const int healed = a.Heal(DiceRoller::Roll(10) + a.sheet.level);
        Say(EventType::Heal, a.id, a.id, Cat(a.Name(), " catches their breath with Second Wind: +", healed, " HP."), 0, 0, healed);
        return {};
    }
    return Fail("unknown feature");
}

}  // namespace dnd
