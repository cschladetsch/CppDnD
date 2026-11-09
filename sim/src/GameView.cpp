// Game: JSON view, save/load and the autopilot.
#include <set>
#include <algorithm>
#include <cctype>
#include <deque>

#include "dnd/AI.hpp"
#include "dnd/Game.hpp"
#include "dnd/Util.hpp"

namespace dnd {
namespace {

Json PartyJson(const PartyMember& pm) {
    const auto& s = pm.sheet;
    Json features = Json::array();
    for (const auto& [id, n] : s.featureUses) features.push_back(Json{{"id", id}, {"uses", n}, {"max", s.featureMax.count(id) ? s.featureMax.at(id) : n}});
    Json spells = Json::array();
    for (const auto& id : s.spells)
        if (const Spell* sp = Content::FindSpell(id)) spells.push_back(Json{{"id", sp->id}, {"name", sp->name}, {"level", sp->level}});
    return Json{{"id", pm.charId}, {"name", s.name}, {"class", s.classId}, {"level", s.level}, {"xp", pm.xp},
                {"nextLevelXp", s.level >= 20 ? 0 : Content::XpForLevel(s.level + 1)}, {"hp", s.hp}, {"maxHp", s.maxHp},
                {"ac", s.armorClass}, {"dead", pm.dead}, {"exhaustion", s.exhaustion}, {"hitDice", s.hitDiceLeft}, {"hitDie", s.hitDie},
                {"potions", s.potions}, {"slots", s.slots}, {"slotsMax", s.slotsMax}, {"features", features}, {"spells", spells}};
}

}  // namespace

Json Game::View(int sinceSeq) const {
    FlushHistory();
    Json v;
    v["mode"] = std::string(ToString(mode_));
    v["role"] = std::string(ToString(cfg_.role));
    v["hero"] = hero_;
    v["pace"] = cfg_.pace;
    v["seed"] = cfg_.seed;
    v["module"] = Json{{"id", module_.id}, {"name", module_.name}, {"setting", module_.setting}};
    v["clock"] = Json{{"hour", hour_}, {"clockHour", ClockHour()}, {"day", Day()}, {"night", module_.IsNight(ClockHour())},
                      {"hoursSinceLongRest", hour_ - lastLongRestHour_ > 100000 ? -1 : hour_ - lastLongRestHour_}};
    v["llm"] = Json{{"dm", cfg_.llmDm}, {"narration", cfg_.llmNarration}, {"agents", cfg_.agents}};

    if (const LocationDef* l = module_.FindLocation(location_)) {
        Json exits = Json::array();
        for (const auto& x : l->exits) {
            const LocationDef* d = module_.FindLocation(x.to);
            const bool open = Satisfied(x.needs);
            Json e{{"to", x.to}, {"name", d ? d->name : x.to}, {"label", x.label}, {"miles", x.miles}, {"hours", x.hours}, {"available", open}};
            if (!open) e["blocked"] = x.blockedText;
            exits.push_back(e);
        }
        Json npcs = Json::array();
        for (const auto& n : l->npcs) {
            Json topics = Json::array();
            for (const auto& t : n.topics)
                if (Satisfied(t.needs)) topics.push_back(Json{{"id", t.id}, {"heard", fired_.count("topic:" + n.id + "/" + t.id) > 0}});
            npcs.push_back(Json{{"id", n.id}, {"name", n.name}, {"type", n.type}, {"description", n.description}, {"topics", topics}});
        }
        v["location"] = Json{{"id", l->id}, {"name", l->name}, {"kind", l->kind}, {"description", l->description}, {"rest", l->rest},
                             {"exits", exits}, {"npcs", npcs}};
    }
    {   // The world map: places the party has been, and the places those lead to (nothing further is revealed).
        std::set<std::string> known = visited_;
        known.insert(location_);
        Json edges = Json::array();
        const std::set<std::string> seen = known;
        for (const auto& id : seen) {
            const LocationDef* l = module_.FindLocation(id);
            if (!l) continue;
            for (const auto& x : l->exits) {
                known.insert(x.to);
                edges.push_back(Json{{"from", id}, {"to", x.to}, {"miles", x.miles}, {"hours", x.hours}, {"open", Satisfied(x.needs)}});
            }
        }
        Json nodes = Json::array();
        for (const auto& id : known) {
            const LocationDef* l = module_.FindLocation(id);
            if (!l) continue;
            nodes.push_back(Json{{"id", id}, {"name", l->name}, {"kind", l->kind}, {"visited", visited_.count(id) > 0}, {"current", id == location_}});
        }
        v["map"] = Json{{"start", module_.start}, {"current", location_}, {"nodes", nodes}, {"edges", edges}};
    }
    if (travel_.active) v["travel"] = Json{{"to", travel_.to}, {"hoursLeft", travel_.hoursLeft}};

    Json party = Json::array();
    for (const auto& pm : party_) party.push_back(PartyJson(pm));
    v["party"] = party;

    Json quests = Json::array();
    for (const auto& q : module_.quests) quests.push_back(Json{{"id", q.id}, {"name", q.name}, {"description", q.description}, {"done", questsDone_.count(q.id) > 0}});
    v["quests"] = quests;

    v["combat"] = combat_ ? CombatView(*combat_) : Json(nullptr);

    Json events = Json::array();
    for (const auto& e : log_)
        if (e.seq > sinceSeq) events.push_back(ToJson(e));
    v["events"] = events;
    v["lastSeq"] = seq_;
    return v;
}

// ---------------------------------------------------------------- persistence

Json Game::Save() const {
    Json j;
    j["version"] = 1;
    j["config"] = Json{{"moduleId", cfg_.moduleId}, {"rooms", cfg_.rooms}, {"difficulty", cfg_.difficulty}, {"party", cfg_.party}, {"level", cfg_.level},
                       {"seed", cfg_.seed}, {"role", std::string(ToString(cfg_.role))}, {"hero", cfg_.hero}, {"pace", cfg_.pace},
                       {"llmDm", cfg_.llmDm}, {"llmNarration", cfg_.llmNarration}, {"agents", cfg_.agents}};
    j["mode"] = static_cast<int>(mode_);
    j["location"] = location_;
    j["hour"] = hour_;
    j["lastLongRestHour"] = lastLongRestHour_;
    j["travelHoursToday"] = travelHoursToday_;
    j["flags"] = flags_;
    j["fired"] = fired_;
    j["visited"] = visited_;
    j["questsDone"] = questsDone_;
    j["queue"] = queue_;
    j["travel"] = Json{{"active", travel_.active}, {"to", travel_.to}, {"hoursLeft", travel_.hoursLeft}, {"table", travel_.table}};
    j["encounter"] = Json{{"monsters", activeEncounter_.monsters}, {"map", activeEncounter_.map}, {"eventId", activeEncounter_.eventId}};
    Json party = Json::array();
    for (const auto& pm : party_) party.push_back(Json{{"charId", pm.charId}, {"sheet", ToJson(pm.sheet)}, {"xp", pm.xp}, {"dead", pm.dead}});
    j["party"] = party;
    j["hero"] = hero_;
    j["history"] = history_.Dump();
    j["rng"] = rng_.State();
    j["seq"] = seq_;
    Json log = Json::array();
    const std::size_t from = log_.size() > 500 ? log_.size() - 500 : 0;
    for (std::size_t i = from; i < log_.size(); ++i) log.push_back(ToJson(log_[i]));
    j["log"] = log;
    j["combat"] = combat_ ? combat_->Save() : Json(nullptr);
    return j;
}

std::unique_ptr<Game> Game::Load(const Json& j) {
    const auto& c = j.at("config");
    GameConfig cfg;
    cfg.moduleId = c.value("moduleId", "");
    cfg.rooms = c.value("rooms", 3);
    cfg.difficulty = c.value("difficulty", "medium");
    cfg.party = c.value("party", std::vector<std::string>{});
    cfg.level = c.value("level", 0);
    cfg.seed = c.value("seed", std::uint64_t{1});
    cfg.role = ParseRole(c.value("role", "player")).value_or(Role::Player);
    cfg.hero = c.value("hero", "");
    cfg.pace = c.value("pace", "normal");
    cfg.llmDm = c.value("llmDm", false);
    cfg.llmNarration = c.value("llmNarration", false);
    cfg.agents = c.value("agents", false);

    std::unique_ptr<Game> g(new Game(cfg, true));
    g->loading_ = true;
    g->mode_ = static_cast<Mode>(j.at("mode").get<int>());
    g->location_ = j.at("location").get<std::string>();
    g->hour_ = j.value("hour", 0);
    g->lastLongRestHour_ = j.value("lastLongRestHour", -1000);
    g->travelHoursToday_ = j.value("travelHoursToday", 0);
    g->flags_ = j.value("flags", std::map<std::string, std::string>{});
    for (const auto& s : j.value("fired", std::vector<std::string>{})) g->fired_.insert(s);
    for (const auto& s : j.value("visited", std::vector<std::string>{})) g->visited_.insert(s);
    for (const auto& s : j.value("questsDone", std::vector<std::string>{})) g->questsDone_.insert(s);
    g->queue_ = j.value("queue", std::vector<std::string>{});
    if (j.contains("travel")) {
        const auto& t = j.at("travel");
        g->travel_ = Pending{t.value("active", false), t.value("to", ""), t.value("hoursLeft", 0), t.value("table", "")};
    }
    if (j.contains("encounter")) {
        const auto& e = j.at("encounter");
        g->activeEncounter_.monsters = e.value("monsters", std::vector<std::string>{});
        g->activeEncounter_.map = e.value("map", std::vector<std::string>{});
        g->activeEncounter_.eventId = e.value("eventId", "");
    }
    for (const auto& p : j.at("party")) {
        PartyMember pm;
        pm.charId = p.value("charId", "");
        pm.sheet = SheetFromJson(p.at("sheet"));
        pm.xp = p.value("xp", 0);
        pm.dead = p.value("dead", false);
        g->party_.push_back(std::move(pm));
    }
    g->hero_ = j.value("hero", "");
    if (j.contains("history")) g->history_ = History::Restore(j.at("history"));
    if (!g->rng_.SetState(j.value("rng", ""))) throw std::runtime_error("save file has a corrupt RNG state");
    g->seq_ = j.value("seq", 0);
    if (j.contains("log"))
        for (const auto& e : j.at("log")) {
            Event ev = EventFromJson(e);
            g->log_.push_back(std::move(ev));
        }
    if (!j.at("combat").is_null()) {
        g->combat_ = std::make_unique<Combat>(TacticalGrid(1, 1), g->bus_);
        g->combat_->Load(j.at("combat"));
        g->combat_->SetRoleSwapHandler([p = g.get()](Role r) {
            p->SetRole(r);
            p->Emit(EventType::Info, Cat("[role is now: ", ToString(r), "]"));
        });
        g->AssignControllers();
    }
    if (!g->module_.FindLocation(g->location_)) throw std::runtime_error("save refers to unknown location '" + g->location_ + "'");
    g->loading_ = false;
    return g;
}

// ---------------------------------------------------------------- autopilot

CmdResult Game::AutoStep() {
    if (mode_ == Mode::Complete || mode_ == Mode::Defeated) return {false, Cat("game is ", ToString(mode_))};

    if (mode_ == Mode::Combat) {
        if (AdvanceCombat() == StepStatus::AwaitingInput && combat_) {
            const int who = combat_->AwaitingActor();
            const GameActor* a = combat_->Find(who);
            AIController ai;
            DiceRoller::Scope scope(rng_);
            const Action act = ai.DecideAction(TurnContext{*combat_, *a});
            SubmitAction(act);
        }
        return {true, "combat"};
    }

    // Exploring. If the party has a council (agents), it decides; with no usable proposal the autopilot below runs.
    if (council_)
        if (auto r = council_(*this)) return *r;
    double frac = 0;
    int n = 0, slots = 0, slotsMax = 0;
    for (const auto& pm : party_) {
        if (pm.dead) continue;
        frac += static_cast<double>(pm.sheet.hp) / pm.sheet.maxHp;
        ++n;
        for (int l = 1; l <= 9; ++l) { slots += pm.sheet.slots[static_cast<std::size_t>(l)]; slotsMax += pm.sheet.slotsMax[static_cast<std::size_t>(l)]; }
    }
    const bool drained = slotsMax > 0 && slots * 2 < slotsMax;
    bool shortRestHelps = false;  // someone is hurt and still has Hit Dice to spend
    for (const auto& pm : party_)
        if (!pm.dead && pm.sheet.hitDiceLeft > 0 && pm.sheet.maxHp - pm.sheet.hp >= pm.sheet.hitDie / 2 + 1 + pm.sheet.Mod(Ability::Con)) shortRestHelps = true;
    if (n && (frac / n < 0.7 || drained) && Here().rest != "none") {
        const bool canLong = hour_ - lastLongRestHour_ >= 24;
        // A short rest never restores spell slots, so repeating it would only burn hours.
        if (canLong || (frac / n < 0.7 && shortRestHelps)) {
            const CmdResult r = Rest(canLong ? "long" : "short");
            if (r.ok) return r;
        }
    }
    for (const auto& npc : Here().npcs)
        for (const auto& t : npc.topics)
            if (Satisfied(t.needs) && !fired_.count("topic:" + npc.id + "/" + t.id)) return Talk(npc.id, t.id);
    auto pendingSearch = [&](const LocationDef& l) {
        for (const auto& e : l.events)
            if (e.trigger == "search" && !fired_.count(l.id + "/" + e.id) && Satisfied(e.needs)) return true;
        return false;
    };
    if (pendingSearch(Here())) return Search();

    // Breadth-first walk to the nearest place with something left to do.
    auto hasWork = [&](const LocationDef& l) {
        if (!visited_.count(l.id) || pendingSearch(l)) return true;
        for (const auto& npc : l.npcs)
            for (const auto& t : npc.topics)
                if (Satisfied(t.needs) && !fired_.count("topic:" + npc.id + "/" + t.id)) return true;
        return false;
    };
    std::map<std::string, std::string> first;  // location -> first hop from here
    std::deque<std::string> q;
    for (const auto& x : Here().exits)
        if (Satisfied(x.needs) && !first.count(x.to)) { first[x.to] = x.to; q.push_back(x.to); }
    while (!q.empty()) {
        const std::string id = q.front();
        q.pop_front();
        const LocationDef* l = module_.FindLocation(id);
        if (l && hasWork(*l)) {
            const CmdResult r = Travel(first[id]);
            return r.ok ? r : CmdResult{false, r.message};
        }
        if (!l) continue;
        for (const auto& x : l->exits)
            if (Satisfied(x.needs) && !first.count(x.to) && x.to != location_) { first[x.to] = first[id]; q.push_back(x.to); }
    }
    return {false, "The autopilot has nothing left to do."};
}

}  // namespace dnd
