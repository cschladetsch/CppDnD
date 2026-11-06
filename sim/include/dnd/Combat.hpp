#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

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
    bool autoCritOnHit = false;  // melee vs a downed creature
    bool sneak = false;
};

enum class Outcome { Victory, Defeat, Timeout };

struct CombatResult {
    Outcome outcome = Outcome::Timeout;
    int rounds = 0;
    int xpPerSurvivor = 0;
};

// Initiative queue, action economy, and rules resolution on a TacticalGrid.
class Combat {
public:
    Combat(TacticalGrid grid, EventBus& bus);

    GameActor& AddActor(Team team, CharacterSheet sheet, Pos pos, std::unique_ptr<IController> controller);
    const std::vector<std::unique_ptr<GameActor>>& Actors() const { return actors_; }
    GameActor* Find(int id);
    const GameActor* Find(int id) const;
    const TacticalGrid& Grid() const { return grid_; }
    int Round() const { return round_; }

    // Queries (used by controllers)
    std::vector<const GameActor*> HostilesOf(const GameActor& self, bool includeDowned) const;
    std::vector<const GameActor*> DownedAlliesOf(const GameActor& self) const;
    std::function<bool(Pos)> PassableFor(const GameActor& mover) const;
    AttackPreview PreviewAttack(const GameActor& a, const GameActor& t, bool ranged, int rangeFt,
                                int longRangeFt, bool sneakCapable) const;
    AttackPreview PreviewWeapon(const GameActor& a, const GameActor& t, const WeaponAttack& w) const;
    static DiceExpr SpellDice(const CharacterSheet& s, const Spell& sp, int slot);
    static int SpellProjectiles(const Spell& sp, int slot);
    static bool WithinBurst(Pos centre, Pos p, int radiusFt);
    std::string RenderMap() const;
    char Glyph(const GameActor& a) const;

    // Execution
    void SetRoleSwapHandler(std::function<void(Role)> h) { roleSwap_ = std::move(h); }
    ActionResult Resolve(GameActor& actor, const Action& action);
    CombatResult Run(int maxRounds = 30);

private:
    struct AttackRollResult { bool hit; bool crit; int natural; int total; int targetAc; };

    void Say(EventType t, int actor, int target, std::string text, int roll = 0, int vs = 0, int amount = 0) const;
    void RollInitiative();
    void RunTurn(GameActor& a);
    void DeathSave(GameActor& a);
    bool CheckEnd(Outcome& out) const;
    CombatResult Finish(Outcome out, int rounds);
    void RemoveFromGrid(GameActor& a);

    ActionResult DoMove(GameActor& a, Pos dest);
    ActionResult DoAttack(GameActor& a, const Action& act);
    ActionResult DoCast(GameActor& a, const Action& act);
    ActionResult DoPotion(GameActor& a, const Action& act);
    void OpportunityAttacks(GameActor& mover, Pos from, Pos to);
    void ResolveWeaponAttack(GameActor& a, GameActor& t, const WeaponAttack& w, const AttackPreview& pv, bool opportunity);
    AttackRollResult RollToHit(int toHit, const GameActor& t, const AttackPreview& pv) const;
    void DealDamage(GameActor& src, GameActor& tgt, int amount, bool crit);
    bool RollSave(const GameActor& t, Ability ab, int dc, int& total) const;

    TacticalGrid grid_;
    EventBus& bus_;
    std::vector<std::unique_ptr<GameActor>> actors_;
    std::vector<int> order_;
    int round_ = 0;
    std::function<void(Role)> roleSwap_;
};

}  // namespace dnd
