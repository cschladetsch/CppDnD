#include "dnd/Game.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>

#include "dnd/AI.hpp"
#include "dnd/Util.hpp"

namespace dnd {
namespace {

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool IsTruthy(const std::string& v) { return !v.empty() && v != "false" && v != "0"; }

}  // namespace

std::string_view ToString(Mode m) {
    switch (m) {
        case Mode::Exploring: return "exploring";
        case Mode::Combat: return "combat";
        case Mode::Complete: return "complete";
        case Mode::Defeated: return "defeated";
    }
    return "?";
}

// ---------------------------------------------------------------- construction

Game::Game(GameConfig cfg) : Game(std::move(cfg), false) {}

Game::Game(GameConfig cfg, bool restore)
    : cfg_(std::move(cfg)), rng_(cfg_.seed), dm_(std::make_unique<AIController>(), cfg_.seed) {
    humanFactory_ = [] { return std::make_unique<RemoteController>(); };
    monsterFactory_ = [] { return std::make_unique<AIController>(); };
    bus_.Subscribe([this](const Event& e) { OnBusEvent(e); });
    Init(restore);
}

Game::~Game() = default;

void Game::Init(bool restore) {
    if (cfg_.moduleId.empty()) {
        module_ = Content::MakeProceduralModule(cfg_.rooms, cfg_.difficulty);
    } else {
        const ModuleDef* m = Content::FindModule(cfg_.moduleId);
        if (!m) throw std::runtime_error("unknown module '" + cfg_.moduleId + "'");
        module_ = *m;
    }
    if (restore) return;

    DiceRoller::Scope scope(rng_);
    const int level = cfg_.level > 0 ? cfg_.level : (cfg_.moduleId.empty() ? 3 : module_.recommendedLevel);
    std::vector<std::string> ids = cfg_.party;
    if (ids.empty()) ids = {"thorin", "elara", "aldric", "finn"};
    for (const auto& id : ids) {
        const CharacterDef* def = Content::FindCharacter(id);
        if (!def) throw std::runtime_error("unknown character '" + id + "'");
        PartyMember pm;
        pm.charId = def->id;
        pm.sheet = Content::BuildCharacter(*def, level);
        pm.xp = Content::XpForLevel(level);
        party_.push_back(std::move(pm));
    }
    hero_ = party_.front().sheet.name;
    if (!cfg_.hero.empty()) {
        const std::string want = Lower(cfg_.hero);
        for (const auto& pm : party_)
            if (Lower(pm.charId) == want || Lower(pm.sheet.name) == want) hero_ = pm.sheet.name;
    }
    for (const auto& d : module_.draws)
        if (!d.choices.empty()) flags_[d.id] = d.choices[static_cast<std::size_t>(DiceRoller::Index(static_cast<int>(d.choices.size())))];

    Emit(EventType::Scene, Cat(module_.name, "\n", module_.intro));
    Arrive(module_.start);
}

// ---------------------------------------------------------------- plumbing

void Game::Emit(EventType t, std::string text, int amount) {
    bus_.Emit(Event{t, -1, -1, 0, 0, amount, std::move(text), 0, {}});
}

void Game::OnBusEvent(const Event& in) {
    Event e = in;
    e.seq = ++seq_;
    if (narrator_ && !loading_) {
        switch (e.type) {
            case EventType::Scene: case EventType::Down: case EventType::Death: case EventType::CombatEnd: case EventType::Quest:
                try { e.flavour = narrator_(e); } catch (...) { e.flavour.clear(); }
                break;
            default: break;
        }
    }
    if (!loading_) RecordEvent(e);
    log_.push_back(e);
    if (log_.size() > 4000) log_.erase(log_.begin(), log_.begin() + 1000);
    if (listener_) listener_(e);
}

void Game::SetHumanFactory(ControllerFactory f) {
    humanFactory_ = std::move(f);
    AssignControllers();
}
void Game::SetMonsterFactory(ControllerFactory f) {
    monsterFactory_ = std::move(f);
    AssignControllers();
}

void Game::SetMonsterControllerFactory(ActorFactory f) {
    monsterFor_ = std::move(f);
    AssignControllers();
}

void Game::SetPartyFactory(PartyFactory f) {
    partyFactory_ = std::move(f);
    AssignControllers();
}

void Game::Say(const std::string& who, const std::string& text, const std::string& to) {
    if (!to.empty()) {
        history_.Between(who, to, "tell", text, Now());
        return;
    }
    Emit(EventType::Narration, Cat(who, ": \"", text, "\""));
    history_.Group(who, "say", text, Now());
}

bool Game::SetHero(const std::string& who, std::string& err) {
    const std::string want = Lower(who);
    const PartyMember* found = nullptr;
    for (const auto& pm : party_)
        if (Lower(pm.charId) == want || Lower(pm.sheet.name) == want) found = &pm;
    if (!found) { err = "nobody in the party is called '" + who + "'"; return false; }
    if (found->dead) { err = found->sheet.name + " is dead"; return false; }
    const bool changed = hero_ != found->sheet.name || cfg_.role != Role::Player;
    hero_ = found->sheet.name;
    cfg_.role = Role::Player;
    AssignControllers();
    if (changed) Emit(EventType::Info, Cat("[you now control ", hero_, "]"));
    return true;
}

std::string Game::ActorName(int id) const {
    if (!combat_ || id < 0) return "";
    for (const auto& a : combat_->Actors())
        if (a->id == id) return a->Name();
    return "";
}

// Mirrors rule events into the histories: who did what to whom (pair files), and what the whole table was told.
void Game::RecordEvent(const Event& e) {
    switch (e.type) {
        case EventType::Attack: case EventType::Spell: case EventType::Heal: {
            const char* kind = e.type == EventType::Attack ? "attack" : e.type == EventType::Spell ? "spell" : "heal";
            history_.Between(ActorName(e.actor), ActorName(e.target), kind, e.text, Now());
            break;
        }
        case EventType::Down: case EventType::Death:  // the event's actor is the victim and its target the one who did it
            history_.Between(ActorName(e.target), ActorName(e.actor), e.type == EventType::Down ? "down" : "death", e.text, Now());
            break;
        case EventType::Scene: history_.Group("DM", "scene", e.text, Now()); break;
        case EventType::Travel: history_.Group("DM", "travel", e.text, Now()); break;
        case EventType::Quest: history_.Group("DM", "quest", e.text, Now()); break;
        case EventType::CombatStart: case EventType::CombatEnd: history_.Group("DM", "combat", e.text, Now()); break;
        default: break;
    }
}

void Game::SetRole(Role r, const std::string& hero) {
    cfg_.role = r;
    if (!hero.empty()) {
        const std::string want = Lower(hero);
        for (const auto& pm : party_)
            if (Lower(pm.charId) == want || Lower(pm.sheet.name) == want) hero_ = pm.sheet.name;
    }
    AssignControllers();
}

void Game::AssignControllers() {
    if (!combat_) return;
    GameActor* heroActor = nullptr;
    for (const auto& a : combat_->Actors())
        if (a->team == Team::Party && !a->IsDead() && Lower(a->Name()) == Lower(hero_)) heroActor = a.get();
    if (!heroActor)
        for (const auto& a : combat_->Actors())
            if (a->team == Team::Party && !a->IsDead()) { heroActor = a.get(); break; }

    for (const auto& a : combat_->Actors()) {
        if (a->team == Team::Party) {
            const bool human = cfg_.role == Role::Player && a.get() == heroActor;
            if (human) a->controller = humanFactory_();
            else if (partyFactory_) a->controller = partyFactory_(a->Name());
            else a->controller = std::make_unique<AIController>();
        } else {
            if (cfg_.role == Role::DungeonMaster) a->controller = humanFactory_();
            else if (monsterFor_) a->controller = monsterFor_(*a);
            else a->controller = monsterFactory_();
        }
    }
}

std::vector<int> Game::LivingLevels() const {
    std::vector<int> v;
    for (const auto& pm : party_)
        if (!pm.dead) v.push_back(pm.sheet.level);
    return v;
}

int Game::LivingCount() const {
    int n = 0;
    for (const auto& pm : party_) n += pm.dead ? 0 : 1;
    return n;
}

double Game::Mph() const { return cfg_.pace == "slow" ? 2.0 : cfg_.pace == "fast" ? 4.0 : 3.0; }

// ---------------------------------------------------------------- flags

bool Game::HasFlag(const std::string& f) const {
    auto it = flags_.find(f);
    return it != flags_.end() && IsTruthy(it->second);
}
std::string Game::FlagValue(const std::string& f) const {
    auto it = flags_.find(f);
    return it == flags_.end() ? "" : it->second;
}

bool Game::Satisfied(const std::vector<std::string>& conds) const {
    for (std::string c : conds) {
        bool neg = false;
        if (!c.empty() && c[0] == '!') { neg = true; c.erase(0, 1); }
        bool ok;
        if (const auto eq = c.find('='); eq != std::string::npos) ok = FlagValue(c.substr(0, eq)) == c.substr(eq + 1);
        else ok = HasFlag(c);
        if (ok == neg) return false;
    }
    return true;
}

void Game::SetFlags(const std::vector<std::string>& flags) {
    for (const auto& f : flags) {
        if (const auto eq = f.find('='); eq != std::string::npos) flags_[f.substr(0, eq)] = f.substr(eq + 1);
        else flags_[f] = "true";
    }
}

void Game::AfterFlagsChanged() {
    for (const auto& q : module_.quests) {
        if (questsDone_.count(q.id) || !Satisfied(q.completeWhen)) continue;
        questsDone_.insert(q.id);
        Emit(EventType::Quest, Cat("Quest complete: ", q.name), q.xp);
        if (q.xp > 0) GiveXp(q.xp, q.name);
    }
    if (mode_ != Mode::Complete && mode_ != Mode::Defeated && !module_.endWhen.empty() && Satisfied(module_.endWhen)) Complete();
}

void Game::GiveXp(int xp, const std::string& why) {
    const int n = LivingCount();
    if (n == 0 || xp <= 0) return;
    for (auto& pm : party_)
        if (!pm.dead) pm.xp += xp / n;
    Emit(EventType::Info, Cat("The party earns ", xp, " XP (", why, ")."), xp);
}

void Game::GivePotions(int n) {
    for (int i = 0; i < n; ++i) {
        PartyMember* best = nullptr;
        for (auto& pm : party_)
            if (!pm.dead && (!best || pm.sheet.potions < best->sheet.potions)) best = &pm;
        if (!best) return;
        ++best->sheet.potions;
        Emit(EventType::Info, Cat(best->sheet.name, " takes a healing potion."));
    }
}

void Game::Complete() {
    if (mode_ == Mode::Complete) return;
    mode_ = Mode::Complete;
    combat_.reset();
    Emit(EventType::Scene, module_.endText.empty() ? "The tale is told." : module_.endText);
}

void Game::Defeat(const std::string& why) {
    mode_ = Mode::Defeated;
    combat_.reset();
    Emit(EventType::Scene, why);
}

// ---------------------------------------------------------------- locations and events

void Game::Arrive(const std::string& locId) {
    location_ = locId;
    const bool first = visited_.insert(locId).second;
    const LocationDef& l = Here();
    Emit(EventType::Scene, first ? Cat(l.name, "\n", l.description) : Cat(l.name));
    QueueEvents("enter");
    RunQueue();
}

void Game::QueueEvents(const std::string& trigger) {
    for (const auto& e : Here().events) {
        if (e.trigger != trigger) continue;
        if (e.once && fired_.count(location_ + "/" + e.id)) continue;
        if (!Satisfied(e.needs)) continue;
        queue_.push_back(e.id);
    }
}

void Game::RunQueue() {
    while (mode_ == Mode::Exploring && !queue_.empty()) {
        const std::string id = queue_.front();
        queue_.erase(queue_.begin());
        const EventDef* ev = nullptr;
        for (const auto& e : Here().events)
            if (e.id == id) ev = &e;
        if (!ev) continue;
        if (ev->once && fired_.count(location_ + "/" + ev->id)) continue;
        if (!Satisfied(ev->needs)) continue;
        FireEvent(*ev);
    }
}

void Game::FireEvent(const EventDef& e) {
    if (e.once) fired_.insert(location_ + "/" + e.id);
    if (!e.narration.empty()) Emit(EventType::Narration, e.narration);
    if (e.encounter) {
        EncounterSpec spec;
        spec.eventId = e.id;
        spec.map = e.map;
        spec.monsters = e.encounter->monsters;
        if (spec.monsters.empty()) {
            const auto d = ParseDifficulty(e.encounter->difficulty).value_or(Difficulty::Medium);
            for (const auto* m : dm_.Generate(LivingLevels(), d).monsters) spec.monsters.push_back(m->id);
        }
        EnterCombat(spec);
    } else {
        FinishEvent(e);
    }
}

void Game::FinishEvent(const EventDef& e) {
    SetFlags(e.setsFlags);
    if (e.potions > 0) GivePotions(e.potions);
    if (e.xp > 0) GiveXp(e.xp, e.id);
    AfterFlagsChanged();
}

std::vector<std::string> Game::Roll(const RandomTableDef& t) {
    int total = 0;
    for (const auto& e : t.entries) total += std::max(1, e.weight);
    int r = DiceRoller::Index(total);
    for (const auto& e : t.entries) {
        r -= std::max(1, e.weight);
        if (r < 0) return e.monsters;
    }
    return t.entries.back().monsters;
}

bool Game::PassHour(const std::string& table, double scale, bool allowEncounter) {
    ++hour_;
    if (!allowEncounter) return false;
    const RandomTableDef* t = module_.FindTable(table.empty() ? Here().randomTable : table);
    if (!t || t->entries.empty()) return false;
    const int pct = static_cast<int>((module_.IsNight(ClockHour()) ? t->chanceNight : t->chanceDay) * scale + 0.5);
    if (DiceRoller::Percentile() > pct) return false;
    EncounterSpec spec;
    spec.monsters = Roll(*t);
    Emit(EventType::Narration, module_.IsNight(ClockHour()) ? "Something stirs in the dark." : "You are not alone.");
    EnterCombat(spec);
    return true;
}

// ---------------------------------------------------------------- travel

CmdResult Game::Travel(const std::string& to) {
    if (mode_ != Mode::Exploring) return {false, Cat("You cannot travel while the game is ", ToString(mode_), ".")};
    DiceRoller::Scope scope(rng_);
    const ExitDef* ex = nullptr;
    for (const auto& x : Here().exits)
        if (Lower(x.to) == Lower(to)) ex = &x;
    if (!ex) return {false, "There is no way to '" + to + "' from here."};
    if (!Satisfied(ex->needs)) {
        const std::string why = ex->blockedText.empty() ? "The way is blocked." : ex->blockedText;
        Emit(EventType::Narration, why);
        return {false, why};
    }
    const LocationDef* dest = module_.FindLocation(ex->to);
    int hours = ex->hours > 0 ? static_cast<int>(std::ceil(ex->hours)) : static_cast<int>(std::ceil(ex->miles / Mph()));
    travel_ = Pending{true, ex->to, hours, ex->table};
    Emit(EventType::Travel, hours > 0 ? Cat("You set out for ", dest->name, " (", hours, " hour(s), ", cfg_.pace, " pace).") : Cat("You head to ", dest->name, "."), hours);
    ContinueTravel();
    return {true, "ok"};
}

void Game::ContinueTravel() {
    const double scale = cfg_.pace == "slow" ? 0.5 : cfg_.pace == "fast" ? 1.5 : 1.0;
    while (travel_.active && travel_.hoursLeft > 0) {
        --travel_.hoursLeft;
        ++travelHoursToday_;
        if (travelHoursToday_ > 8) {  // forced march: CON save DC 10 + hours past 8
            const int dc = 10 + travelHoursToday_ - 8;
            for (auto& pm : party_) {
                if (pm.dead) continue;
                const int roll = DiceRoller::D20().natural + pm.sheet.SaveBonus(Ability::Con);
                if (roll < dc) {
                    ++pm.sheet.exhaustion;
                    Emit(EventType::Rest, Cat(pm.sheet.name, " falters on the forced march (", roll, " vs DC ", dc, "), exhaustion ", pm.sheet.exhaustion, "."));
                    if (pm.sheet.exhaustion >= 6) { pm.dead = true; pm.sheet.hp = 0; Emit(EventType::Death, Cat(pm.sheet.name, " dies of exhaustion.")); }
                }
            }
            if (LivingCount() == 0) { Defeat("The party perishes on the road."); return; }
        }
        if (PassHour(travel_.table, scale, true)) return;  // combat started; the trip resumes afterwards
    }
    if (!travel_.active) return;
    const std::string dest = travel_.to;
    travel_ = Pending{};
    Arrive(dest);
}

CmdResult Game::SetPace(const std::string& pace) {
    if (pace != "slow" && pace != "normal" && pace != "fast") return {false, "pace must be slow, normal or fast"};
    cfg_.pace = pace;
    return {true, "ok"};
}

// ---------------------------------------------------------------- search, talk, wait

CmdResult Game::Search() {
    if (mode_ != Mode::Exploring) return {false, "You cannot search right now."};
    DiceRoller::Scope scope(rng_);
    Emit(EventType::Narration, "You spend an hour searching the area.");
    if (PassHour("", 1.0, Here().rest != "safe")) return {true, "interrupted"};
    const auto before = fired_.size();
    QueueEvents("search");
    RunQueue();
    if (fired_.size() == before && mode_ == Mode::Exploring) Emit(EventType::Narration, "You find nothing new.");
    return {true, "ok"};
}

CmdResult Game::Talk(const std::string& npcId, const std::string& topicId) {
    if (mode_ != Mode::Exploring) return {false, "Not now."};
    DiceRoller::Scope scope(rng_);
    const NpcDef* npc = nullptr;
    for (const auto& n : Here().npcs)
        if (Lower(n.id) == Lower(npcId)) npc = &n;
    if (!npc) return {false, "Nobody called '" + npcId + "' is here."};
    if (topicId.empty()) {
        std::string list;
        for (const auto& t : npc->topics)
            if (Satisfied(t.needs)) list += (list.empty() ? "" : ", ") + t.id;
        return {true, Cat(npc->name, " can speak of: ", list)};
    }
    const TopicDef* topic = nullptr;
    for (const auto& t : npc->topics)
        if (Lower(t.id) == Lower(topicId)) topic = &t;
    if (!topic || !Satisfied(topic->needs)) return {false, Cat(npc->name, " has nothing to say about that.")};
    std::string spoken;
    if (npcVoice_) {
        try { spoken = npcVoice_(*npc, *topic); } catch (...) { spoken.clear(); }
    }
    history_.Between(hero_, npc->name, "ask", Cat("asks about ", topic->id), Now());
    history_.Between(npc->name, hero_, "reply", spoken.empty() ? topic->text : spoken, Now());
    Emit(EventType::Narration, Cat(npc->name, ": ", spoken.empty() ? topic->text : spoken));
    const std::string key = "topic:" + npc->id + "/" + topic->id;
    if (fired_.insert(key).second) {
        SetFlags(topic->setsFlags);
        if (topic->potions > 0) GivePotions(topic->potions);
        AfterFlagsChanged();
    }
    return {true, "ok"};
}

CmdResult Game::Wait(int hours) {
    if (mode_ != Mode::Exploring) return {false, "Not now."};
    DiceRoller::Scope scope(rng_);
    hours = std::clamp(hours, 1, 24);
    Emit(EventType::Travel, Cat("You wait ", hours, " hour(s)."));
    for (int i = 0; i < hours; ++i)
        if (PassHour("", 1.0, Here().rest != "safe")) break;
    return {true, "ok"};
}

// ---------------------------------------------------------------- rests

void Game::ApplyShortRest(int maxDice) {
    for (auto& pm : party_) {
        if (pm.dead) continue;
        auto& s = pm.sheet;
        int spent = 0, healed = 0;
        const int con = s.Mod(Ability::Con);
        while (spent < maxDice && s.hitDiceLeft > 0 && s.hp < s.maxHp && s.maxHp - s.hp >= s.hitDie / 2 + 1 + con) {
            const int gain = std::max(0, DiceRoller::Roll(s.hitDie) + con);
            const int real = std::min(gain, s.maxHp - s.hp);
            s.hp += real;
            healed += real;
            --s.hitDiceLeft;
            ++spent;
        }
        for (const char* f : {"second_wind"})
            if (s.featureMax.count(f)) s.featureUses[f] = s.featureMax[f];
        Emit(EventType::Rest, Cat(s.name, " spends ", spent, " hit dice and recovers ", healed, " HP (", s.hp, "/", s.maxHp, ")."));
    }
}

void Game::CheckLevelUps() {
    for (auto& pm : party_) {
        if (pm.dead) continue;
        const CharacterDef* def = Content::FindCharacter(pm.charId);
        const int target = std::min(20, Content::LevelForXp(pm.xp));
        if (!def || target <= pm.sheet.level) continue;
        const int potions = pm.sheet.potions, exhaustion = pm.sheet.exhaustion;
        pm.sheet = Content::BuildCharacter(*def, target);
        pm.sheet.potions = potions;
        pm.sheet.exhaustion = exhaustion;
        Emit(EventType::Rest, Cat(pm.sheet.name, " reaches level ", target, "!"), target);
    }
}

void Game::ApplyLongRest() {
    for (auto& pm : party_) {
        if (pm.dead) continue;
        auto& s = pm.sheet;
        s.hp = s.maxHp;
        s.slots = s.slotsMax;
        s.featureUses = s.featureMax;
        s.hitDiceLeft = std::min(s.level, s.hitDiceLeft + std::max(1, s.level / 2));
        if (s.exhaustion > 0) --s.exhaustion;
    }
    lastLongRestHour_ = hour_;
    travelHoursToday_ = 0;
    Emit(EventType::Rest, "The party completes a long rest: hit points, spell slots and class features restored.");
    CheckLevelUps();
}

CmdResult Game::Rest(const std::string& kind, int maxDice) {
    if (mode_ != Mode::Exploring) return {false, "You cannot rest now."};
    DiceRoller::Scope scope(rng_);
    const LocationDef& loc = Here();
    if (loc.rest == "none") {
        Emit(EventType::Rest, "This is no place to rest.");
        return {false, "This is no place to rest."};
    }
    const bool risky = loc.rest != "safe";
    if (kind == "short") {
        if (PassHour("", 1.0, risky)) { Emit(EventType::Rest, "The rest is interrupted."); return {true, "interrupted"}; }
        ApplyShortRest(maxDice);
        return {true, "ok"};
    }
    if (kind == "long") {
        if (hour_ - lastLongRestHour_ < 24) {
            const std::string m = Cat("You have rested too recently; wait ", 24 - (hour_ - lastLongRestHour_), " more hour(s).");
            Emit(EventType::Rest, m);
            return {false, m};
        }
        for (int i = 0; i < 8; ++i)
            if (PassHour("", 1.0, risky)) { Emit(EventType::Rest, "The long rest is interrupted and brings no benefit."); return {true, "interrupted"}; }
        ApplyLongRest();
        return {true, "ok"};
    }
    return {false, "rest kind must be short or long"};
}

// ---------------------------------------------------------------- combat

void Game::EnterCombat(const EncounterSpec& spec) {
    activeEncounter_ = spec;
    mode_ = Mode::Combat;

    std::vector<std::string> rows = !spec.map.empty() ? spec.map : (!Here().map.empty() ? Here().map : Content::DefaultRoomMap());
    std::size_t w = 0;
    for (const auto& r : rows) w = std::max(w, r.size());
    TacticalGrid grid(static_cast<int>(w), static_cast<int>(rows.size()));
    std::vector<Pos> pCells, mCells;
    for (int y = 0; y < grid.Height(); ++y)
        for (int x = 0; x < grid.Width(); ++x) {
            const char c = static_cast<std::size_t>(x) < rows[static_cast<std::size_t>(y)].size() ? rows[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] : '#';
            switch (c) {
                case '#': grid.Set({x, y}, Terrain::Wall); break;
                case '~': grid.Set({x, y}, Terrain::Difficult); break;
                case 'c': grid.Set({x, y}, Terrain::HalfCover); break;
                case 'O': grid.Set({x, y}, Terrain::ThreeQuarterCover); break;
                case 'P': pCells.push_back({x, y}); break;
                case 'M': mCells.push_back({x, y}); break;
                default: break;
            }
        }

    combat_ = std::make_unique<Combat>(std::move(grid), bus_);
    combat_->SetRoleSwapHandler([this](Role r) {
        SetRole(r);
        Emit(EventType::Info, Cat("[role is now: ", ToString(r), "]"));
    });
    combat_->SetMaxRounds(40);
    const TacticalGrid& g = combat_->Grid();

    auto fill = [&](std::vector<Pos>& cells, std::size_t need, Pos anchor) {
        std::vector<Pos> extra;
        for (int y = 0; y < g.Height(); ++y)
            for (int x = 0; x < g.Width(); ++x) {
                const Pos p{x, y};
                if (!g.IsWalkableTerrain(p) || g.OccupantAt(p) >= 0) continue;
                if (std::find(cells.begin(), cells.end(), p) != cells.end()) continue;
                extra.push_back(p);
            }
        std::stable_sort(extra.begin(), extra.end(), [&](Pos a, Pos b) { return DistanceFt(a, anchor) < DistanceFt(b, anchor); });
        for (const Pos& p : extra) {
            if (cells.size() >= need) break;
            cells.push_back(p);
        }
    };

    std::vector<const PartyMember*> living;
    for (const auto& pm : party_)
        if (!pm.dead) living.push_back(&pm);
    fill(pCells, living.size(), Pos{1, g.Height() / 2});
    for (std::size_t i = 0; i < living.size() && i < pCells.size(); ++i) combat_->AddActor(Team::Party, living[i]->sheet, pCells[i], nullptr);

    std::vector<const MonsterDef*> defs;
    for (const auto& id : spec.monsters) {
        const MonsterDef* m = Content::FindMonster(id);
        if (!m) throw std::runtime_error("unknown monster '" + id + "' in event");
        defs.push_back(m);
    }
    fill(mCells, defs.size(), Pos{g.Width() - 2, g.Height() / 2});
    for (std::size_t i = mCells.size(); i > 1; --i) std::swap(mCells[i - 1], mCells[static_cast<std::size_t>(DiceRoller::Index(static_cast<int>(i)))]);
    std::map<std::string, int> total, seen;
    for (const auto* m : defs) ++total[m->name];
    std::string names;
    for (std::size_t i = 0; i < defs.size() && i < mCells.size(); ++i) {
        CharacterSheet sheet = defs[i]->sheet;
        sheet.name = total[defs[i]->name] > 1 ? Cat(defs[i]->name, " ", ++seen[defs[i]->name]) : defs[i]->name;
        names += (names.empty() ? "" : ", ") + sheet.name;
        combat_->AddActor(Team::Monsters, std::move(sheet), mCells[i], nullptr);
    }
    Emit(EventType::Scene, Cat("Danger! ", names, "."));
    AssignControllers();
}

StepStatus Game::AdvanceCombat() {
    if (!combat_) return StepStatus::Finished;
    DiceRoller::Scope scope(rng_);
    const StepStatus st = combat_->Advance();
    if (st == StepStatus::Finished) OnCombatEnd();
    return st;
}

ActionResult Game::SubmitAction(const Action& a) {
    if (!combat_) return {false, "there is no fight"};
    DiceRoller::Scope scope(rng_);
    const int who = combat_->AwaitingActor();
    if (who < 0) return {false, "no input is being awaited"};
    return combat_->Submit(who, a);
}

void Game::OnCombatEnd() {
    DiceRoller::Scope scope(rng_);
    const CombatResult res = combat_->Result();
    // Fold the fight back into the persistent party.
    for (const auto& a : combat_->Actors()) {
        if (a->team != Team::Party) continue;
        for (auto& pm : party_) {
            if (pm.dead || pm.sheet.name != a->Name()) continue;
            pm.sheet = a->sheet;
            if (a->life == LifeState::Dead) { pm.dead = true; pm.sheet.hp = 0; }
            else if (a->life != LifeState::Alive || pm.sheet.hp <= 0) pm.sheet.hp = 1;
            pm.sheet.economy = ActionEconomy{};
        }
    }
    combat_.reset();

    if (res.outcome == Outcome::Defeat || LivingCount() == 0) {
        Defeat("The party has fallen. The story ends here.");
        return;
    }
    if (res.outcome == Outcome::Timeout) {
        Emit(EventType::Info, "The fight drags on until the party slips away; no spoils are won.");
        mode_ = Mode::Exploring;
        if (!activeEncounter_.eventId.empty()) fired_.erase(location_ + "/" + activeEncounter_.eventId);
    } else {
        mode_ = Mode::Exploring;
        GiveXp(res.xpTotal, "victory");
        if (!activeEncounter_.eventId.empty())
            for (const auto& e : Here().events)
                if (e.id == activeEncounter_.eventId) { FinishEvent(e); break; }
    }
    activeEncounter_ = EncounterSpec{};
    if (mode_ != Mode::Exploring) return;
    if (travel_.active) ContinueTravel();
    else RunQueue();
}

}  // namespace dnd
