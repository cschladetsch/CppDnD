#pragma once
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dnd/Game.hpp"
#include "dnd/Json.hpp"
#include "dnd/Llm.hpp"

namespace dnd {

// ---------------------------------------------------------------- configuration

// Where a model is served. Several agents can share one backend.
struct BackendDef {
    std::string url = "http://127.0.0.1:11434";
    std::string path = "/v1/chat/completions";
    std::string apiKey;
    bool stream = false;
    int timeoutSec = 120;
};

// One actor's mind: which backend and model it uses, and how it behaves.
struct AgentDef {
    std::string backend = "ollama";
    std::string model;
    std::string persona;
    double temperature = 0.7;
    int maxTokens = 400;
    bool enabled = true;  // false: that actor falls back to the built-in AI
    bool requireStore = false;  // the model must exist in the CppLmmModelStore (checked when the agent is made or switched)
};

// The global agents file (data/agents.json, --agents, DND_AGENTS). Players inherit defaults, then
// players["*"], then players[<character id or name>]; the DM inherits defaults, then dm; an NPC inherits
// defaults, then npcTypes["*"], then npcTypes[<its type>], then npcs[<its id or name>].
struct AgentsConfig {
    std::map<std::string, BackendDef> backends;
    AgentDef dm;
    AgentDef playerFallback;
    std::map<std::string, AgentDef> players;  // lowercase character id or name
    AgentDef npcFallback;                     // defaults + npcTypes["*"]
    std::map<std::string, AgentDef> npcTypes; // by NPC type, each already merged with npcFallback
    std::map<std::string, Json> npcOverrides; // by lowercase NPC id or name, applied on top of the type
    AgentDef monsterFallback;                 // dm + monsterTypes["*"]
    std::map<std::string, AgentDef> monsterTypes;     // by creature type tag (undead, beast, humanoid, ...), merged with monsterFallback
    std::map<std::string, Json> monsterOverrides;     // by monster id (goblin, ghoul, ...), applied on top of its type

    static std::optional<AgentsConfig> Parse(const Json& j, std::string& err);
    static std::optional<AgentsConfig> Load(const std::string& path, std::string& err);
    AgentDef ForPlayer(const std::string& charId, const std::string& charName) const;
    // Which mind plays this monster: {selector, definition}. Selector is "monster:<id>", "type:<tag>" or "dm".
    std::pair<std::string, AgentDef> ForMonster(const std::string& monsterId, const std::vector<std::string>& tags) const;
    AgentDef ForNpc(const std::string& npcId, const std::string& npcName, const std::string& type) const;
    const BackendDef* FindBackend(const std::string& name) const;
    LlmConfig ToLlmConfig(const AgentDef& d) const;
};

// ---------------------------------------------------------------- JSON messaging

struct AgentMessage {
    int seq = 0;
    std::string from;
    std::string to;    // "all", an agent name, or "log" (recorded, delivered to nobody)
    std::string kind;  // say | narrate | proposal | decision
    Json body;
};

// The table everybody talks across. Agents never see each other's prompts, only what is posted here.
class AgentHub {
public:
    using Observer = std::function<void(const AgentMessage&)>;
    void Post(std::string from, std::string to, std::string kind, Json body);
    // Called for every message as it is posted (the game uses this to keep its histories).
    void SetObserver(Observer o) { observer_ = std::move(o); }
    // Unread messages for `agent` (not its own), advancing `cursor`. At most the last 12.
    Json Inbox(const std::string& agent, int& cursor) const;
    Json LogJson(int sinceSeq = 0) const;
    int LastSeq() const { return seq_; }

private:
    std::vector<AgentMessage> log_;
    int seq_ = 0;
    Observer observer_;
};

// ---------------------------------------------------------------- agent

// An independent mind: own client (so own model), own persona, own short memory. Every exchange is
// a JSON document in and a JSON document out.
class Agent {
public:
    using SayFn = std::function<void(const std::string& who, const std::string& text)>;
    Agent(std::string name, std::string role, AgentDef def, std::shared_ptr<ILlmClient> client, std::shared_ptr<AgentHub> hub, SayFn say = nullptr);

    // Sends {task, observation, table (unread messages), recent decisions, reply_schema}; returns the parsed
    // JSON reply. Posts any "say" and "propose" to the table. After repeated failures it backs off for a while
    // so a dead model cannot stall the game.
    std::optional<Json> Ask(const std::string& task, const Json& observation, std::string* err = nullptr);
    void Announce(const std::string& text);  // DM narration, delivered to everyone

    const std::string& Name() const { return name_; }
    const std::string& Role() const { return role_; }
    const AgentDef& Def() const { return def_; }
    std::shared_ptr<ILlmClient> Client() const { return client_; }
    int Calls() const { return calls_; }
    int Failures() const { return failures_; }
    void SetSay(SayFn s) { say_ = std::move(s); }
    // What this agent remembers of past conversations: called as history(name) before each ask, sent as "history".
    using HistoryFn = std::function<Json(const std::string& who)>;
    void SetHistory(HistoryFn f) { history_ = std::move(f); }
    // Switches the model (and client) this agent uses; memory and what it has heard are kept.
    void Rebind(AgentDef def, std::shared_ptr<ILlmClient> client) { def_ = std::move(def); client_ = std::move(client); streak_ = skip_ = 0; }

private:
    std::string System() const;
    std::string name_, role_;
    AgentDef def_;
    std::shared_ptr<ILlmClient> client_;
    std::shared_ptr<AgentHub> hub_;
    SayFn say_;
    HistoryFn history_;
    int cursor_ = 0, calls_ = 0, failures_ = 0, streak_ = 0, skip_ = 0;
    std::vector<Json> memory_;
};

// What an agent sees when it must act in a fight, as JSON (never anything it could not see).
Json ObserveTurn(const TurnContext& ctx);

// Drives one actor in combat through its agent. Illegal, malformed or missing replies fall back to the
// built-in AI for the rest of that turn, so the rules engine stays authoritative.
class AgentTurnController : public IController {
public:
    explicit AgentTurnController(std::shared_ptr<Agent> agent);
    Action DecideAction(const TurnContext& ctx) override;
    std::unique_ptr<IController> Clone() const override { return std::make_unique<AgentTurnController>(agent_); }
    std::string_view Kind() const override { return "Agent"; }
    void OnActionResult(const TurnContext&, const Action&, bool ok, const std::string&) override;

private:
    std::shared_ptr<Agent> agent_;
    std::unique_ptr<IController> fallback_;
    int planRound_ = -1, planActor_ = -1;
    std::vector<Action> plan_;
    bool useFallback_ = false;
};

// ---------------------------------------------------------------- the table of agents for one game

class AgentTable {
public:
    using ClientFactory = std::function<std::shared_ptr<ILlmClient>(const std::string& agentName, const BackendDef&, const AgentDef&)>;
    // factory == nullptr: a real HttpLlmClient per agent.
    explicit AgentTable(AgentsConfig cfg, ClientFactory factory = nullptr);

    // Wires the game: the DM agent plays the monsters and narrates, each player agent plays its own party
    // member (except a human hero), and the party decides its exploration moves by talking it over.
    static void Attach(const std::shared_ptr<AgentTable>& table, Game& game);

    std::shared_ptr<Agent> Dm();
    std::shared_ptr<Agent> Player(const std::string& charId, const std::string& charName);  // null when disabled
    std::optional<CmdResult> Convene(Game& game);
    // The mind that plays this monster: its own per-monster-id agent, else its creature type's, else the DM's.
    std::shared_ptr<Agent> MonsterAgent(const GameActor& monster);
    // Switch the model one actor (or a whole kind of enemy) uses, now and for the rest of the game.
    // selector: "dm", "player:<name>", "npc:<id>", "type:<creature tag>", "monster:<monster id>".
    // fromStore: the model must exist in the CppLmmModelStore. The backend serves the model; the store resolves and checks it.
    bool SetModel(const std::string& selector, const std::string& model, const std::string& backend, bool fromStore, std::string& err);
    std::shared_ptr<Agent> Npc(const NpcDef& npc);  // null when disabled or no model is set
    std::string Speak(Game& game, const NpcDef& npc, const TopicDef& topic);  // empty: use the written text
    Json Describe() const;
    Json Problems() const;  // agents that could not be made (e.g. a model required from the store is missing)
    AgentHub& Hub() { return *hub_; }
    const AgentsConfig& Config() const { return cfg_; }

private:
    std::shared_ptr<Agent> Make(const std::string& name, const std::string& role, const AgentDef& def);
    std::shared_ptr<ILlmClient> MakeClient(const std::string& name, const AgentDef& def);
    void Remember(const std::string& selector, const std::shared_ptr<Agent>& a);
    std::shared_ptr<Agent> Find(const std::string& selector) const;
    AgentsConfig cfg_;
    ClientFactory factory_;
    std::shared_ptr<AgentHub> hub_;
    Agent::SayFn say_;
    Agent::HistoryFn history_;
    std::shared_ptr<Agent> dm_;
    bool dmMade_ = false;
    std::map<std::string, std::shared_ptr<Agent>> players_;
    std::vector<std::string> playerOrder_;
    std::map<std::string, std::shared_ptr<Agent>> npcs_;
    std::vector<std::string> npcOrder_;
    std::map<std::string, std::shared_ptr<Agent>> monsters_;    // "type:<tag>" and "monster:<id>" minds (null: disabled)
    std::vector<std::string> monsterOrder_;
    std::map<std::string, std::pair<std::string, std::string>> overrides_;  // selector -> {model, backend}, from SetModel
    std::vector<std::string> problems_;
};

}  // namespace dnd
