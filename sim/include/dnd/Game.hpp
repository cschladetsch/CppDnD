#pragma once
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "dnd/Combat.hpp"
#include "dnd/Content.hpp"
#include "dnd/DM.hpp"
#include "dnd/Events.hpp"
#include "dnd/History.hpp"
#include "dnd/Json.hpp"

namespace dnd {

struct GameConfig {
    std::string moduleId;             // empty: a procedural delve
    int rooms = 3;                    // procedural only
    std::string difficulty = "medium";  // procedural only
    std::vector<std::string> party;   // character ids; empty: thorin, elara, aldric, finn
    int level = 0;                    // 0: the module's recommendedLevel
    std::uint64_t seed = 1;
    Role role = Role::Player;
    std::string hero;                 // character id or name; empty: first party member
    std::string pace = "normal";      // slow | normal | fast
    bool llmDm = false;               // recorded so a loaded game knows how it was wired
    bool llmNarration = false;
    bool agents = false;              // every actor is its own model-driven agent (see Agents.hpp)
};

enum class Mode { Exploring, Combat, Complete, Defeated };
std::string_view ToString(Mode);

struct CmdResult {
    bool ok = true;
    std::string message;
};

struct PartyMember {
    std::string charId;
    CharacterSheet sheet;
    int xp = 0;
    bool dead = false;
};

// A long-running session: a party moving through a module (graph of locations on a clock), with
// tactical fights, rests, travel, NPC talk, flags and quests. Everything is step-driven so a web
// backend can drive it request by request, and the whole thing saves to and loads from JSON.
//
// Thread-safety: none. The server serialises access per game. Every mutating call installs this
// game's own RNG, so concurrent games are independent and a loaded game replays identically.
class Game {
public:
    using Narrator = std::function<std::string(const Event&)>;
    using ControllerFactory = std::function<std::unique_ptr<IController>()>;
    using Listener = std::function<void(const Event&)>;
    using PartyFactory = std::function<std::unique_ptr<IController>(const std::string& heroName)>;
    // Decides the party's next exploration move for AutoStep; nullopt means "no opinion", and the built-in autopilot runs.
    // Speaks a topic in the NPC's own voice; an empty result means "use the written text".
    // Picks the controller for one monster (so each kind of enemy can have its own mind); overrides the plain factory.
    using ActorFactory = std::function<std::unique_ptr<IController>(const GameActor&)>;
    using NpcVoice = std::function<std::string(const NpcDef&, const TopicDef&)>;
    using Council = std::function<std::optional<CmdResult>(Game&)>;

    explicit Game(GameConfig cfg);  // throws std::runtime_error on unknown ids
    ~Game();
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;

    // ---- wiring (re-applied to a running fight)
    void SetNarrator(Narrator n) { narrator_ = std::move(n); }
    void SetHumanFactory(ControllerFactory f);    // default: RemoteController (never blocks)
    void SetMonsterFactory(ControllerFactory f);  // default: AIController; an LLM DM plugs in here
    void SetListener(Listener l) { listener_ = std::move(l); }
    void SetPartyFactory(PartyFactory f);         // default: AIController for every non-human party member
    void SetCouncil(Council c) { council_ = std::move(c); }
    void SetNpcVoice(NpcVoice v) { npcVoice_ = std::move(v); }
    void SetMonsterControllerFactory(ActorFactory f);
    void SetRole(Role r, const std::string& hero = "");
    // Change which party member the user controls (and make the user a player). Fails for an unknown or dead member.
    bool SetHero(const std::string& who, std::string& err);
    const std::string& Hero() const { return hero_; }
    // Table talk. Empty `to`: said to the whole table (shown in the log, kept in group history). Otherwise a
    // private line to one actor, kept only in that pair's histories.
    void Say(const std::string& who, const std::string& text, const std::string& to = "");
    // ---- history (see History.hpp)
    History& Hist() { return history_; }
    const History& Hist() const { return history_; }
    void SetHistoryDir(const std::string& dir) { history_.SetDir(dir); }
    void FlushHistory() const { history_.Flush(); }
    Stamp Now() const { return Stamp{Day(), ClockHour()}; }

    // ---- exploration
    CmdResult Travel(const std::string& to);
    CmdResult Search();
    CmdResult Talk(const std::string& npc, const std::string& topic);
    CmdResult Rest(const std::string& kind, int maxHitDice = 99);  // "short" | "long"
    CmdResult Wait(int hours);
    CmdResult SetPace(const std::string& pace);

    // ---- combat
    StepStatus AdvanceCombat();                 // runs until input is needed or the fight ends
    ActionResult SubmitAction(const Action& a);
    // Plays the next sensible step on behalf of the party (used for spectator mode and tests).
    CmdResult AutoStep();

    // ---- queries
    Mode GetMode() const { return mode_; }
    Role GetRole() const { return cfg_.role; }
    const GameConfig& Config() const { return cfg_; }
    const ModuleDef& Module() const { return module_; }
    const std::vector<PartyMember>& Party() const { return party_; }
    const Combat* GetCombat() const { return combat_.get(); }
    Combat* GetCombat() { return combat_.get(); }
    int Hour() const { return hour_; }
    int ClockHour() const { return (module_.startHour + hour_) % 24; }
    int Day() const { return (module_.startHour + hour_) / 24 + 1; }
    const std::string& LocationId() const { return location_; }
    bool HasFlag(const std::string& f) const;
    std::string FlagValue(const std::string& f) const;
    const std::vector<Event>& Log() const { return log_; }
    int LastSeq() const { return seq_; }
    Json View(int sinceSeq = 0) const;

    // ---- persistence
    Json Save() const;
    static std::unique_ptr<Game> Load(const Json& j);

private:
    Game(GameConfig cfg, bool restore);
    void Init(bool restore);

    struct Pending {
        bool active = false;
        std::string to;
        int hoursLeft = 0;
        std::string table;
    };
    struct EncounterSpec {
        std::vector<std::string> monsters;
        std::vector<std::string> map;
        std::string eventId;   // event to finalise on victory, empty for a random encounter
        std::string label;
    };

    // clock and rests
    bool PassHour(const std::string& table, double chanceScale, bool allowEncounter);
    void ApplyShortRest(int maxDice);
    void ApplyLongRest();
    void CheckLevelUps();
    CmdResult ForcedMarchChecks();

    // module logic
    bool Satisfied(const std::vector<std::string>& conds) const;
    void SetFlags(const std::vector<std::string>& flags);
    void AfterFlagsChanged();
    void Arrive(const std::string& locId);
    void QueueEvents(const std::string& trigger);
    void RunQueue();
    void FireEvent(const EventDef& e);
    void FinishEvent(const EventDef& e);
    void GiveXp(int xp, const std::string& why);
    void GivePotions(int n);
    std::vector<std::string> Roll(const RandomTableDef& t);
    void ContinueTravel();
    void EnterCombat(const EncounterSpec& spec);
    void OnCombatEnd();
    void Complete();
    void Defeat(const std::string& why);

    // plumbing
    void Emit(EventType t, std::string text, int amount = 0);
    void OnBusEvent(const Event& e);
    void AssignControllers();
    std::vector<int> LivingLevels() const;
    const LocationDef& Here() const { return *module_.FindLocation(location_); }
    double Mph() const;
    int LivingCount() const;

    std::string ActorName(int combatId) const;
    void RecordEvent(const Event& e);

    GameConfig cfg_;
    ModuleDef module_;
    Rng rng_;
    EventBus bus_;
    Narrator narrator_;
    Listener listener_;
    ControllerFactory humanFactory_, monsterFactory_;
    PartyFactory partyFactory_;
    Council council_;
    NpcVoice npcVoice_;
    ActorFactory monsterFor_;
    DungeonMasterEngine dm_;

    Mode mode_ = Mode::Exploring;
    std::vector<PartyMember> party_;
    std::string location_;
    int hour_ = 0;
    int lastLongRestHour_ = -1000;
    int travelHoursToday_ = 0;
    std::map<std::string, std::string> flags_;
    std::set<std::string> fired_;      // "<location>/<event>", "topic:<npc>/<topic>"
    std::set<std::string> visited_;
    std::set<std::string> questsDone_;
    std::vector<std::string> queue_;   // event ids still to run at the current location
    Pending travel_;
    EncounterSpec activeEncounter_;
    std::unique_ptr<Combat> combat_;
    std::string hero_;                 // name of the human-controlled hero

    mutable History history_;  // mutable so View() can flush files
    std::vector<Event> log_;
    int seq_ = 0;
    bool loading_ = false;
};

}  // namespace dnd
