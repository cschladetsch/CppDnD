#include "dnd/Agents.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "dnd/AI.hpp"
#include "dnd/Content.hpp"
#include "dnd/Util.hpp"

namespace dnd {

namespace {

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Overlays only the keys present in `j` onto `d`.
void Overlay(AgentDef& d, const Json& j) {
    if (!j.is_object()) return;
    if (j.contains("backend")) d.backend = j.at("backend").get<std::string>();
    if (j.contains("model")) d.model = j.at("model").get<std::string>();
    if (j.contains("persona")) d.persona = j.at("persona").get<std::string>();
    if (j.contains("temperature")) d.temperature = j.at("temperature").get<double>();
    if (j.contains("maxTokens")) d.maxTokens = j.at("maxTokens").get<int>();
    if (j.contains("enabled")) d.enabled = j.at("enabled").get<bool>();
    if (j.contains("requireStore")) d.requireStore = j.at("requireStore").get<bool>();
}

Json Schema(const std::string& task) {
    if (task == "choose_party_move")
        return Json{{"say", "optional: one short in-character line to the party (max 25 words)"},
                    {"propose", Json{{"cmd", "travel|search|talk|rest|wait|pace"}, {"to", "exit id (travel)"}, {"npc", "npc id (talk)"}, {"topic", "topic id (talk)"},
                                     {"kind", "short|long (rest)"}, {"hours", "number (wait)"}}},
                    {"note", "propose exactly one move for the whole party; prefer unvisited places and unheard topics; use only ids from the observation"}};
    if (task == "speak")
        return Json{{"say", "what you say aloud, in character, 1 to 3 sentences (max 60 words)"},
                    {"note", "convey the facts and nothing more; do not reveal anything not in facts; do not describe other people's actions"}};
    return Json{{"say", "optional: one short in-character line (max 25 words)"},
                {"actions", Json::array({Json{{"type", "move"}, {"dest", "[x,y]"}}, Json{{"type", "attack"}, {"target", "creature id"}, {"attack", "attack index"}},
                                         Json{{"type", "cast"}, {"spell", "spell id"}, {"target", "creature id"}, {"slot", "level"}}, Json{{"type", "dash"}},
                                         Json{{"type", "dodge"}}, Json{{"type", "potion"}, {"target", "creature id"}}, Json{{"type", "feature"}, {"feature", "id"}},
                                         Json{{"type", "end_turn"}}})},
                {"note", "plan this one turn, at most 4 actions, ending with end_turn; melee needs adjacency so move first; use only ids from the observation"}};
}

}  // namespace

// ---------------------------------------------------------------- configuration

std::optional<AgentsConfig> AgentsConfig::Parse(const Json& j, std::string& err) {
    try {
        if (!j.is_object()) { err = "the agents file must be a JSON object"; return std::nullopt; }
        AgentsConfig c;
        if (j.contains("backends")) {
            for (auto it = j["backends"].begin(); it != j["backends"].end(); ++it) {
                BackendDef b;
                const Json& o = it.value();
                b.url = o.value("url", b.url);
                b.path = o.value("path", b.path);
                b.apiKey = o.value("apiKey", b.apiKey);
                b.stream = o.value("stream", b.stream);
                b.timeoutSec = o.value("timeoutSec", b.timeoutSec);
                c.backends[it.key()] = b;
            }
        }
        if (c.backends.empty()) c.backends["ollama"] = BackendDef{};

        AgentDef defaults;
        if (c.backends.count(defaults.backend) == 0) defaults.backend = c.backends.begin()->first;
        if (j.contains("defaults")) Overlay(defaults, j["defaults"]);

        c.dm = defaults;
        if (j.contains("dm")) Overlay(c.dm, j["dm"]);

        c.playerFallback = defaults;
        const Json empty = Json::object();
        const Json& pl = j.contains("players") ? j["players"] : empty;
        if (pl.contains("*")) Overlay(c.playerFallback, pl["*"]);
        for (auto it = pl.begin(); it != pl.end(); ++it) {
            if (it.key() == "*") continue;
            AgentDef d = c.playerFallback;
            Overlay(d, it.value());
            c.players[Lower(it.key())] = d;
        }

        c.npcFallback = defaults;
        const Json& nt = j.contains("npcTypes") ? j["npcTypes"] : empty;
        if (nt.contains("*")) Overlay(c.npcFallback, nt["*"]);
        for (auto it = nt.begin(); it != nt.end(); ++it) {
            if (it.key() == "*") continue;
            AgentDef d = c.npcFallback;
            Overlay(d, it.value());
            c.npcTypes[Lower(it.key())] = d;
        }
        if (j.contains("npcs"))
            for (auto it = j["npcs"].begin(); it != j["npcs"].end(); ++it) {
                if (!it.value().is_object()) { err = "npcs." + it.key() + " must be an object"; return std::nullopt; }
                c.npcOverrides[Lower(it.key())] = it.value();
            }

        c.monsterFallback = c.dm;
        const Json& mt = j.contains("monsterTypes") ? j["monsterTypes"] : empty;
        if (mt.contains("*")) Overlay(c.monsterFallback, mt["*"]);
        for (auto it = mt.begin(); it != mt.end(); ++it) {
            if (it.key() == "*") continue;
            AgentDef d = c.monsterFallback;
            Overlay(d, it.value());
            c.monsterTypes[Lower(it.key())] = d;
        }
        if (j.contains("monsters"))
            for (auto it = j["monsters"].begin(); it != j["monsters"].end(); ++it) {
                if (!it.value().is_object()) { err = "monsters." + it.key() + " must be an object"; return std::nullopt; }
                c.monsterOverrides[Lower(it.key())] = it.value();
            }

        auto check = [&](const std::string& who, const AgentDef& d) {
            if (!d.enabled) return true;
            if (c.backends.count(d.backend) == 0) { err = who + ": unknown backend '" + d.backend + "'"; return false; }
            if (d.model.empty()) { err = who + ": no model set (give it one, or set \"enabled\": false)"; return false; }
            return true;
        };
        if (!check("dm", c.dm)) return std::nullopt;
        if (!check("players.*", c.playerFallback)) return std::nullopt;
        for (const auto& [k, d] : c.players)
            if (!check("players." + k, d)) return std::nullopt;
        if (!check("monsterTypes.*", c.monsterFallback)) return std::nullopt;
        for (const auto& [k, d] : c.monsterTypes)
            if (!check("monsterTypes." + k, d)) return std::nullopt;
        for (const auto& [k, o] : c.monsterOverrides) {
            AgentDef d = c.monsterFallback;
            Overlay(d, o);
            if (!check("monsters." + k, d)) return std::nullopt;
        }
        // NPC models are optional: an NPC with no model simply speaks its written lines.
        auto checkNpc = [&](const std::string& who, const AgentDef& d) {
            if (!d.enabled) return true;
            if (c.backends.count(d.backend) == 0) { err = who + ": unknown backend '" + d.backend + "'"; return false; }
            return true;
        };
        if (!checkNpc("npcTypes.*", c.npcFallback)) return std::nullopt;
        for (const auto& [k, d] : c.npcTypes)
            if (!checkNpc("npcTypes." + k, d)) return std::nullopt;
        for (const auto& [k, o] : c.npcOverrides) {
            AgentDef d = c.npcFallback;
            Overlay(d, o);
            if (!checkNpc("npcs." + k, d)) return std::nullopt;
        }
        return c;
    } catch (const Json::exception& e) {
        err = std::string("invalid agents file: ") + e.what();
        return std::nullopt;
    }
}

std::optional<AgentsConfig> AgentsConfig::Load(const std::string& path, std::string& err) {
    std::ifstream in(path);
    if (!in) { err = "cannot open " + path; return std::nullopt; }
    const Json j = Json::parse(in, nullptr, false, true);  // comments allowed
    if (j.is_discarded()) { err = path + " is not valid JSON"; return std::nullopt; }
    auto c = Parse(j, err);
    if (!c) err = path + ": " + err;
    return c;
}

AgentDef AgentsConfig::ForPlayer(const std::string& charId, const std::string& charName) const {
    if (auto it = players.find(Lower(charId)); it != players.end()) return it->second;
    if (auto it = players.find(Lower(charName)); it != players.end()) return it->second;
    return playerFallback;
}

std::pair<std::string, AgentDef> AgentsConfig::ForMonster(const std::string& monsterId, const std::vector<std::string>& tags) const {
    AgentDef d = monsterFallback;
    std::string selector = "dm";
    for (const auto& t : tags)
        if (auto it = monsterTypes.find(Lower(t)); it != monsterTypes.end()) { d = it->second; selector = "type:" + Lower(t); break; }
    if (auto it = monsterOverrides.find(Lower(monsterId)); it != monsterOverrides.end()) {
        Overlay(d, it->second);
        selector = "monster:" + Lower(monsterId);
    }
    return {selector, d};
}

AgentDef AgentsConfig::ForNpc(const std::string& npcId, const std::string& npcName, const std::string& type) const {
    AgentDef d = npcFallback;
    if (auto it = npcTypes.find(Lower(type)); it != npcTypes.end()) d = it->second;
    if (auto it = npcOverrides.find(Lower(npcId)); it != npcOverrides.end()) Overlay(d, it->second);
    else if (auto it2 = npcOverrides.find(Lower(npcName)); it2 != npcOverrides.end()) Overlay(d, it2->second);
    return d;
}

const BackendDef* AgentsConfig::FindBackend(const std::string& name) const {
    const auto it = backends.find(name);
    return it == backends.end() ? nullptr : &it->second;
}

LlmConfig AgentsConfig::ToLlmConfig(const AgentDef& d) const {
    LlmConfig c;
    if (const BackendDef* b = FindBackend(d.backend)) {
        c.baseUrl = b->url;
        c.path = b->path;
        c.apiKey = b->apiKey;
        c.stream = b->stream;
        c.timeoutSec = b->timeoutSec;
    }
    c.model = d.model;
    c.temperature = d.temperature;
    c.maxTokens = d.maxTokens;
    return c;
}

// ---------------------------------------------------------------- hub

void AgentHub::Post(std::string from, std::string to, std::string kind, Json body) {
    log_.push_back(AgentMessage{++seq_, std::move(from), std::move(to), std::move(kind), std::move(body)});
    if (log_.size() > 2000) log_.erase(log_.begin(), log_.begin() + 500);
}

Json AgentHub::Inbox(const std::string& agent, int& cursor) const {
    Json out = Json::array();
    for (const auto& m : log_) {
        if (m.seq <= cursor || m.from == agent || m.kind == "decision") continue;
        if (m.to != "all" && m.to != agent) continue;
        out.push_back(Json{{"from", m.from}, {"kind", m.kind}, {"body", m.body}});
    }
    cursor = seq_;
    while (out.size() > 12) out.erase(out.begin());
    return out;
}

Json AgentHub::LogJson(int sinceSeq) const {
    Json out = Json::array();
    for (const auto& m : log_)
        if (m.seq > sinceSeq) out.push_back(Json{{"seq", m.seq}, {"from", m.from}, {"to", m.to}, {"kind", m.kind}, {"body", m.body}});
    return out;
}

// ---------------------------------------------------------------- agent

Agent::Agent(std::string name, std::string role, AgentDef def, std::shared_ptr<ILlmClient> client, std::shared_ptr<AgentHub> hub, SayFn say)
    : name_(std::move(name)), role_(std::move(role)), def_(std::move(def)), client_(std::move(client)), hub_(std::move(hub)), say_(std::move(say)) {}

std::string Agent::System() const {
    std::string s;
    if (role_ == "dm")
        s = "You are the Dungeon Master's tactical mind in a D&D 5e game, playing the monsters. You are an independent agent: you see only the JSON you are given. "
            "Play each monster as it would really fight: smart ones focus the weak and flee when losing, beasts go for the nearest. Do not be cruel for its own sake.";
    else if (role_ == "npc")
        s = Cat("You are ", name_, ", a non-player character in a D&D 5e game, played by an independent mind. You speak only in your own voice, never as the narrator. "
                "You know only what the JSON tells you.");
    else
        s = Cat("You are ", name_, ", a player character in a D&D 5e party, played by an independent mind. You see only the JSON you are given and you act only for ",
                name_, ". Your companions are separate agents: you can talk to them through \"say\", and their words arrive in \"table\".");
    if (!def_.persona.empty()) s += " Your personality: " + def_.persona;
    s += " Reply with ONE JSON object matching reply_schema and nothing else. Use only ids that appear in the observation. Never invent creatures, places or items.";
    return s;
}

std::optional<Json> Agent::Ask(const std::string& task, const Json& observation, std::string* errOut) {
    auto fail = [&](const std::string& e) -> std::optional<Json> {
        if (errOut) *errOut = e;
        ++failures_;
        if (++streak_ >= 2) skip_ = 10;
        return std::nullopt;
    };
    if (skip_ > 0) {
        --skip_;
        if (errOut) *errOut = "backing off after repeated failures";
        return std::nullopt;
    }
    ++calls_;
    Json msg;
    msg["task"] = task;
    msg["you"] = name_;
    msg["role"] = role_;
    msg["table"] = hub_->Inbox(name_, cursor_);
    if (!memory_.empty()) msg["your_recent_decisions"] = memory_;
    msg["observation"] = observation;
    msg["reply_schema"] = Schema(task);
    const LlmReply r = client_->Chat(System(), msg.dump(), true);
    if (!r.ok) return fail(r.error);
    const std::string obj = ExtractJsonObject(r.content);
    Json j = obj.empty() ? Json() : Json::parse(obj, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return fail("reply was not a JSON object");
    streak_ = 0;

    Json mem = j;
    mem.erase("say");
    memory_.push_back(Json{{"task", task}, {"decision", mem}});
    if (memory_.size() > 3) memory_.erase(memory_.begin());
    hub_->Post(name_, "log", "decision", j);
    if (j.contains("propose") && j["propose"].is_object()) hub_->Post(name_, "all", "proposal", j["propose"]);
    if (j.contains("say") && j["say"].is_string()) {
        std::string text = j["say"].get<std::string>();
        if (text.size() > 300) text.resize(300);
        std::replace(text.begin(), text.end(), '\n', ' ');
        if (!text.empty()) {
            hub_->Post(name_, "all", "say", Json{{"text", text}});
            if (say_) say_(name_, text);
        }
    }
    return j;
}

void Agent::Announce(const std::string& text) { hub_->Post(name_, "all", "narrate", Json{{"text", text}}); }

// ---------------------------------------------------------------- combat

Json ObserveTurn(const TurnContext& ctx) {
    const GameActor& me = ctx.self;
    const auto& s = me.sheet;
    Json o;
    o["round"] = ctx.combat.Round();

    Json atk = Json::array();
    for (std::size_t i = 0; i < s.attacks.size(); ++i) {
        const auto& w = s.attacks[i];
        atk.push_back(Json{{"index", i}, {"name", w.name}, {"toHit", w.toHit}, {"damage", w.damage.ToString()}, {"rangeFt", w.rangeFt}, {"longRangeFt", w.longRangeFt}});
    }
    Json spells = Json::array();
    for (const auto& id : s.spells)
        if (const Spell* x = Content::FindSpell(id))
            spells.push_back(Json{{"id", x->id}, {"name", x->name}, {"level", x->level}, {"rangeFt", x->rangeFt},
                                  {"available", x->level == 0 || s.LowestSlotAtLeast(x->level) > 0}});
    Json features = Json::array();
    for (const auto& [id, n] : s.featureUses) features.push_back(Json{{"id", id}, {"uses", n}});
    o["you"] = Json{{"id", me.id}, {"name", me.Name()}, {"pos", Json::array({me.pos.x, me.pos.y})}, {"hp", s.hp}, {"maxHp", s.maxHp}, {"ac", me.EffectiveAc()},
                    {"movementFt", s.economy.movementFt}, {"action", s.economy.action}, {"bonusAction", s.economy.bonusAction},
                    {"attacksLeft", s.economy.attacksLeft}, {"potions", s.potions}, {"attacks", atk}, {"spells", spells}, {"features", features}};

    const auto& g = ctx.combat.Grid();
    Json rows = Json::array();
    for (int y = 0; y < g.Height(); ++y) {
        std::string row;
        for (int x = 0; x < g.Width(); ++x) {
            switch (g.At({x, y})) {
                case Terrain::Wall: row += '#'; break;
                case Terrain::HalfCover: row += 'c'; break;
                case Terrain::ThreeQuarterCover: row += 'O'; break;
                case Terrain::Difficult: row += '~'; break;
                default: row += '.'; break;
            }
        }
        rows.push_back(row);
    }
    o["map"] = Json{{"legend", "x grows right, y grows down, 5 ft per square; # wall, c half cover, O three-quarters cover, ~ rough"}, {"rows", rows}};
    Json who = Json::array();
    for (const auto& a : ctx.combat.Actors()) {
        if (a->IsDead()) continue;
        who.push_back(Json{{"id", a->id}, {"name", a->Name()}, {"side", a->team == me.team ? "ally" : "enemy"}, {"pos", Json::array({a->pos.x, a->pos.y})},
                           {"hp", a->sheet.hp}, {"maxHp", a->sheet.maxHp}, {"ac", a->EffectiveAc()}, {"down", a->IsDown()}});
    }
    o["creatures"] = who;
    return o;
}

AgentTurnController::AgentTurnController(std::shared_ptr<Agent> agent) : agent_(std::move(agent)), fallback_(std::make_unique<AIController>()) {}

Action AgentTurnController::DecideAction(const TurnContext& ctx) {
    if (ctx.combat.Round() != planRound_ || ctx.self.id != planActor_) {
        planRound_ = ctx.combat.Round();
        planActor_ = ctx.self.id;
        plan_.clear();
        useFallback_ = false;
        const auto reply = agent_->Ask("take_turn", ObserveTurn(ctx));
        std::string err;
        if (reply) plan_ = LlmDmController::ParsePlan(reply->dump(), err);
        if (!reply || plan_.empty()) useFallback_ = true;
        std::reverse(plan_.begin(), plan_.end());  // pop from the back
    }
    if (useFallback_) return fallback_->DecideAction(ctx);
    if (plan_.empty()) return Action::End();
    Action a = plan_.back();
    plan_.pop_back();
    return a;
}

void AgentTurnController::OnActionResult(const TurnContext&, const Action&, bool ok, const std::string&) {
    if (ok || useFallback_) return;
    plan_.clear();
    useFallback_ = true;  // the rest of this turn is played by the built-in AI
}

// ---------------------------------------------------------------- table

AgentTable::AgentTable(AgentsConfig cfg, ClientFactory factory)
    : cfg_(std::move(cfg)), factory_(std::move(factory)), hub_(std::make_shared<AgentHub>()) {}

std::shared_ptr<ILlmClient> AgentTable::MakeClient(const std::string& name, const AgentDef& def) {
    if (factory_) return factory_(name, *cfg_.FindBackend(def.backend), def);
    return std::make_shared<HttpLlmClient>(cfg_.ToLlmConfig(def));
}

std::shared_ptr<Agent> AgentTable::Make(const std::string& name, const std::string& role, const AgentDef& def) {
    if (!def.enabled || def.model.empty()) return nullptr;
    if (def.requireStore && !LocateModel(def.model).exists) {
        problems_.push_back(Cat(name, ": model '", def.model, "' is not in the model store (", LocateModel(def.model).path, "); using the built-in AI"));
        return nullptr;
    }
    return std::make_shared<Agent>(name, role, def, MakeClient(name, def), hub_, role == "npc" ? nullptr : say_);
}

std::shared_ptr<Agent> AgentTable::Dm() {
    if (!dmMade_) {
        AgentDef d = cfg_.dm;
        if (auto it = overrides_.find("dm"); it != overrides_.end()) { d.model = it->second.first; if (!it->second.second.empty()) d.backend = it->second.second; }
        dm_ = Make("DM", "dm", d);
        dmMade_ = true;
    }
    return dm_;
}

std::shared_ptr<Agent> AgentTable::Player(const std::string& charId, const std::string& charName) {
    const std::string key = Lower(charName);
    if (auto it = players_.find(key); it != players_.end()) return it->second;
    AgentDef pd = cfg_.ForPlayer(charId, charName);
    if (auto it = overrides_.find("player:" + key); it != overrides_.end()) { pd.model = it->second.first; if (!it->second.second.empty()) pd.backend = it->second.second; }
    auto a = Make(charName, "player", pd);
    players_[key] = a;
    playerOrder_.push_back(key);
    return a;
}

std::shared_ptr<Agent> AgentTable::MonsterAgent(const GameActor& m) {
    auto [sel, def] = cfg_.ForMonster(m.sheet.monsterId, m.sheet.tags);
    const std::string idSel = "monster:" + Lower(m.sheet.monsterId);
    if (!m.sheet.monsterId.empty() && overrides_.count(idSel)) sel = idSel;  // a runtime switch can add selectors the file does not mention
    else if (sel == "dm")
        for (const auto& t : m.sheet.tags)
            if (overrides_.count("type:" + Lower(t))) { sel = "type:" + Lower(t); break; }
    if (sel == "dm") return Dm();
    if (auto it = monsters_.find(sel); it != monsters_.end()) return it->second;
    if (auto it = overrides_.find(sel); it != overrides_.end()) { def.model = it->second.first; if (!it->second.second.empty()) def.backend = it->second.second; }
    std::string label = sel.substr(sel.find(':') + 1);
    if (sel.rfind("monster:", 0) == 0) { if (const MonsterDef* md = Content::FindMonster(m.sheet.monsterId)) label = md->name; }
    else if (!label.empty()) label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
    def.persona = Cat("You command every ", label, sel.rfind("type:", 0) == 0 ? " (a kind of creature)" : "", " in the fight. ", def.persona);
    auto a = Make(label, "dm", def);
    Remember(sel, a);
    return a;
}

void AgentTable::Remember(const std::string& selector, const std::shared_ptr<Agent>& a) {
    monsters_[selector] = a;
    monsterOrder_.push_back(selector);
}

std::shared_ptr<Agent> AgentTable::Find(const std::string& selector) const {
    if (selector == "dm") return dm_;
    if (selector.rfind("player:", 0) == 0) {
        const auto it = players_.find(Lower(selector.substr(7)));
        return it == players_.end() ? nullptr : it->second;
    }
    if (selector.rfind("npc:", 0) == 0) {
        const auto it = npcs_.find(Lower(selector.substr(4)));
        return it == npcs_.end() ? nullptr : it->second;
    }
    const auto it = monsters_.find(selector);
    return it == monsters_.end() ? nullptr : it->second;
}

bool AgentTable::SetModel(const std::string& selector, const std::string& model, const std::string& backend, bool fromStore, std::string& err) {
    const std::string sel = Lower(selector);
    if (model.empty()) { err = "give a model name"; return false; }
    if (!backend.empty() && !cfg_.FindBackend(backend)) { err = "unknown backend '" + backend + "'"; return false; }
    bool known = sel == "dm" || sel.rfind("player:", 0) == 0 || sel.rfind("npc:", 0) == 0;
    if (sel.rfind("monster:", 0) == 0) known = Content::FindMonster(sel.substr(8)) != nullptr;
    if (sel.rfind("type:", 0) == 0) {
        const std::string tag = sel.substr(5);
        for (const auto& m : Content::Monsters())
            for (const auto& t : m.sheet.tags) if (Lower(t) == tag) known = true;
    }
    if (!known) { err = "unknown selector '" + selector + "' (use dm, player:<name>, npc:<id>, type:<creature type> or monster:<id>)"; return false; }
    if (fromStore) {
        const ModelInfo mi = LocateModel(model);
        if (!mi.exists) { err = Cat("model '", model, "' is not in the model store (looked in ", mi.path, ")"); return false; }
    }
    overrides_[sel] = {model, backend};
    if (auto a = Find(sel)) {
        AgentDef d = a->Def();
        d.model = model;
        if (!backend.empty()) d.backend = backend;
        a->Rebind(d, MakeClient(a->Name(), d));
    }
    return true;
}

std::shared_ptr<Agent> AgentTable::Npc(const NpcDef& npc) {
    const std::string key = Lower(npc.id);
    if (auto it = npcs_.find(key); it != npcs_.end()) return it->second;
    AgentDef def = cfg_.ForNpc(npc.id, npc.name, npc.type);
    if (auto it = overrides_.find("npc:" + key); it != overrides_.end()) { def.model = it->second.first; if (!it->second.second.empty()) def.backend = it->second.second; }
    if (def.persona.empty()) def.persona = Cat("A ", npc.type, ". ", npc.description);
    else def.persona = Cat(def.persona, " (", npc.type, ". ", npc.description, ")");
    auto a = Make(npc.name, "npc", def);
    npcs_[key] = a;
    npcOrder_.push_back(key);
    return a;
}

std::string AgentTable::Speak(Game& g, const NpcDef& npc, const TopicDef& topic) {
    auto a = Npc(npc);
    if (!a) return {};
    Json party = Json::array();
    for (const auto& pm : g.Party())
        if (!pm.dead) party.push_back(Json{{"name", pm.sheet.name}, {"class", pm.sheet.classId}});
    const Json obs{{"npc", Json{{"name", npc.name}, {"type", npc.type}, {"description", npc.description}}},
                   {"location", g.Module().FindLocation(g.LocationId()) ? g.Module().FindLocation(g.LocationId())->name : g.LocationId()},
                   {"setting", g.Module().setting}, {"night", g.Module().IsNight(g.ClockHour())},
                   {"speaking_to", party}, {"topic", topic.id}, {"facts", topic.text}};
    const auto reply = a->Ask("speak", obs);
    if (!reply || !reply->contains("say") || !(*reply)["say"].is_string()) return {};
    std::string text = (*reply)["say"].get<std::string>();
    if (text.size() > 500) text.resize(500);
    std::replace(text.begin(), text.end(), '\n', ' ');
    return text;
}

std::optional<CmdResult> AgentTable::Convene(Game& g) {
    if (g.GetMode() != Mode::Exploring) return std::nullopt;
    std::vector<std::shared_ptr<Agent>> voters;
    for (const auto& pm : g.Party())
        if (!pm.dead)
            if (auto a = Player(pm.charId, pm.sheet.name)) voters.push_back(a);
    if (voters.empty()) return std::nullopt;

    const Json world = ObserveWorld(g);
    struct Cand { Json step; int votes = 0; };
    std::vector<Cand> cands;
    for (const auto& v : voters) {  // in order: later speakers hear earlier ones through the table
        const auto reply = v->Ask("choose_party_move", world);
        if (!reply || !reply->contains("propose") || !(*reply)["propose"].is_object()) continue;
        const Json& step = (*reply)["propose"];
        auto it = std::find_if(cands.begin(), cands.end(), [&](const Cand& c) { return c.step == step; });
        if (it != cands.end()) ++it->votes;
        else cands.push_back(Cand{step, 1});
    }
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.votes > b.votes; });  // ties: first to speak wins
    for (const auto& c : cands) {
        const CmdResult r = RunExplorationStep(g, c.step);
        if (r.ok) return r;
    }
    return std::nullopt;  // nobody gave a usable proposal: the built-in autopilot decides
}

namespace {
// Forwards to whatever client the agent has right now, so a model switch also reaches the narrator.
class AgentClient : public ILlmClient {
public:
    explicit AgentClient(std::shared_ptr<Agent> a) : a_(std::move(a)) {}
    LlmReply Chat(const std::string& system, const std::string& user, bool json) override { return a_->Client()->Chat(system, user, json); }
    std::string Describe() const override { return a_->Client()->Describe(); }

private:
    std::shared_ptr<Agent> a_;
};
}  // namespace

void AgentTable::Attach(const std::shared_ptr<AgentTable>& t, Game& g) {
    t->say_ = [&g](const std::string& who, const std::string& text) { g.Say(who, text); };
    // Agents created before Attach (or by tests) pick up the same speaker.
    for (auto& [k, a] : t->players_) if (a) a->SetSay(t->say_);
    if (t->dm_) t->dm_->SetSay(t->say_);
    for (const auto& pm : g.Party()) t->Player(pm.charId, pm.sheet.name);  // create them all now, so they are visible

    if (auto dm = t->Dm()) {
        g.SetNarrator([dm, inner = MakeLlmNarrator(std::make_shared<AgentClient>(dm), g)](const Event& e) {
            std::string text = inner(e);
            if (!text.empty()) dm->Announce(text);
            return text;
        });
    }
    // Each kind of enemy gets its own mind (and so its own model) when agents.json says so; the rest are the DM's.
    g.SetMonsterControllerFactory([t](const GameActor& m) -> std::unique_ptr<IController> {
        if (auto a = t->MonsterAgent(m)) return std::make_unique<AgentTurnController>(a);
        return std::make_unique<AIController>();
    });
    g.SetPartyFactory([t, &g](const std::string& name) -> std::unique_ptr<IController> {
        for (const auto& pm : g.Party())
            if (pm.sheet.name == name)
                if (auto a = t->Player(pm.charId, name)) return std::make_unique<AgentTurnController>(a);
        return std::make_unique<AIController>();
    });
    g.SetCouncil([t](Game& game) { return t->Convene(game); });
    g.SetNpcVoice([t, &g](const NpcDef& n, const TopicDef& tp) { return t->Speak(g, n, tp); });
}

Json AgentTable::Describe() const {
    Json out = Json::array();
    auto add = [&](const std::string& selector, const std::shared_ptr<Agent>& a) {
        if (!a) return;
        const ModelInfo mi = LocateModel(a->Def().model);
        const BackendDef* b = cfg_.FindBackend(a->Def().backend);
        out.push_back(Json{{"name", a->Name()}, {"role", a->Role()}, {"selector", selector}, {"backend", a->Def().backend}, {"url", b ? b->url : ""}, {"model", a->Def().model},
                           {"persona", a->Def().persona}, {"temperature", a->Def().temperature}, {"calls", a->Calls()}, {"failures", a->Failures()},
                           {"modelStore", Json{{"path", mi.path}, {"exists", mi.exists}}}});
    };
    add("dm", dm_);
    for (const auto& k : monsterOrder_) add(k, monsters_.at(k));
    for (const auto& k : playerOrder_) add("player:" + k, players_.at(k));
    for (const auto& k : npcOrder_) add("npc:" + k, npcs_.at(k));
    return out;
}

Json AgentTable::Problems() const { return problems_; }

}  // namespace dnd
