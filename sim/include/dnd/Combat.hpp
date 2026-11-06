#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "dnd/Actor.hpp"
#include "dnd/Controller.hpp"
#include "dnd/Events.hpp"
#include "dnd/Grid.hpp"

namespace dnd {

struct ActionResult {
    bool ok = true;
    std::string reason;
};

// What the rules say about a prospective attack. Shared by the resolver and the AI so
// both always agree on advantage, cover and range.
struct AttackPreview {
    bool legal = false;
    std::string whyNot;
    int distanceFt = 0;
    Cover cover = Cover::None;
    int coverBonus = 0;
    RollMode mode = RollMode::Normal;
    bool autoCritOnHit = false;  // melee vs a downed or paralyzed creature
    bool sneak = false;
};

enum class Outcome { Victory, Defeat, Timeout };
enum class StepStatus { AwaitingInput, Finished };

struct CombatResult {
    Outcome outcome = Outcome::Timeout;
    int rounds = 0;
    int xpPerSurvivor = 0;
    int xpTotal = 0;
};

// Initiative queue, action economy and rules resolution on a TacticalGrid.
// Execution is resumable: Advance() runs until a RemoteController needs an answer (or the fight
// ends), Submit() supplies that answer. The whole state can be saved and restored as JSON.
class Combat {
public:
    Combat(TacticalGrid grid, EventBus& bus);

    GameActor& AddActor(Team team, CharacterSheet sheet, Pos pos, std::unique_ptr<IController> controller);
    void ClearActors();
    const std::vector<std::unique_ptr<GameActor>>& Actors() const { return actors_; }
    GameActor* Find(int id);
    const GameActor* Find(int id) const;
    const TacticalGrid& Grid() const { return grid_; }
    int Round() const { return round_; }
    const std::vector<int>& Order() const { return order_; }
    int CurrentActorId() const { return inTurn_ && idx_ < order_.size() ? order_[idx_] : -1; }
    bool Started() const { return started_; }

    // ---- queries (used by controllers)
    std::vector<const GameActor*> HostilesOf(const GameActor& self, bool includeDowned) const;
    std::vector<const GameActor*> DownedAlliesOf(const GameActor& self) const;
    std::function<bool(Pos)> PassableFor(const GameActor& mover) const;
    AttackPreview PreviewAttack(const GameActor& a, const GameActor& t, bool ranged, int rangeFt,
                                int longRangeFt, bool sneakCapable) const;
    AttackPreview PreviewWeapon(const GameActor& a, const GameActor& t, const WeaponAttack& w) const;
    static DiceExpr SpellDice(const CharacterSheet& s, const Spell& sp, int slot);
    static int SpellProjectiles(const Spell& sp, int slot);
    static bool WithinBurst(Pos centre, Pos p, int radiusFt);
    static int AdjustDamage(const GameActor& target, int amount, DamageType type, bool magical);
    std::string RenderMap() const;
    char Glyph(const GameActor& a) const;

    // ---- execution
    void SetRoleSwapHandler(std::function<void(Role)> h) { roleSwap_ = std::move(h); }
    void SetMaxRounds(int n) { maxRounds_ = n; }
    ActionResult Resolve(GameActor& actor, const Action& action);
    StepStatus Advance();
    int AwaitingActor() const { return awaiting_; }
    ActionResult Submit(int actorId, const Action& action);  // answer to a RemoteController
    bool IsFinished() const { return finished_; }
    const CombatResult& Result() const { return result_; }
    CombatResult Run(int maxRounds = 30);  // blocking convenience for local controllers

    // ---- persistence (Json.cpp)
    nlohmann::json Save() const;
    void Load(const nlohmann::json& j);  // actors only; controllers are re-attached by the caller

private:
    struct AttackRollResult { bool hit; bool crit; int natural; int total; int targetAc; };

    void Say(EventType t, int actor, int target, std::string text, int roll = 0, int vs = 0, int amount = 0) const;
    void RollInitiative();
    void BeginTurn(GameActor& a);
    void FinishTurn(GameActor& a);
    ActionResult StepAction(GameActor& a, const Action& act, bool countInvalid);
    void DeathSave(GameActor& a);
    bool CheckEnd(Outcome& out) const;
    void Finish(Outcome out);
    void RemoveFromGrid(GameActor& a);

    ActionResult DoMove(GameActor& a, Pos dest);
    ActionResult DoAttack(GameActor& a, const Action& act);
    ActionResult DoCast(GameActor& a, const Action& act);
    ActionResult DoPotion(GameActor& a, const Action& act);
    ActionResult DoFeature(GameActor& a, const Action& act);
    void OpportunityAttacks(GameActor& mover, Pos from, Pos to);
    void ResolveWeaponAttack(GameActor& a, GameActor& t, const WeaponAttack& w, const AttackPreview& pv, bool opportunity);
    AttackRollResult RollToHit(const GameActor& a, int toHit, const GameActor& t, const AttackPreview& pv);
    void DealDamage(GameActor& src, GameActor& tgt, int amount, bool crit);
    bool RollSave(const GameActor& t, Ability ab, int dc, int& total, int coverBonus = 0) const;

    // conditions and concentration
    void ApplyEffect(GameActor& target, Effect e);
    void RemoveEffect(GameActor& target, std::size_t index, const char* why);
    void EndConcentration(GameActor& caster);
    void TickEndOfTurn(GameActor& a);
    void ConsumeAdvantageEffects(GameActor& target);

    TacticalGrid grid_;
    EventBus& bus_;
    std::vector<std::unique_ptr<GameActor>> actors_;
    std::vector<int> order_;
    std::function<void(Role)> roleSwap_;

    // resumable turn state
    bool started_ = false, finished_ = false, inTurn_ = false;
    int round_ = 0, maxRounds_ = 30;
    std::size_t idx_ = 0;
    int invalid_ = 0, steps_ = 0, awaiting_ = -1;
    CombatResult result_;
};

}  // namespace dnd
