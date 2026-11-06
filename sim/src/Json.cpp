#include "dnd/Json.hpp"

#include <algorithm>

#include "dnd/Content.hpp"

namespace dnd {
namespace {

std::string CondName(Condition c) { return c == Condition::None ? "" : std::string(ToString(c)); }
Condition CondFrom(const std::string& s) {
    if (s.empty()) return Condition::None;
    return ParseCondition(s).value_or(Condition::None);
}

template <class T>
Json ListOf(const std::vector<T>& v, std::string (*f)(const T&)) {
    Json a = Json::array();
    for (const auto& x : v) a.push_back(f(x));
    return a;
}

Json Damages(const std::vector<DamageType>& v) {
    Json a = Json::array();
    for (auto d : v) a.push_back(std::string(ToString(d)));
    return a;
}
std::vector<DamageType> DamagesFrom(const Json& j) {
    std::vector<DamageType> v;
    for (const auto& x : j)
        if (auto d = ParseDamageType(x.get<std::string>())) v.push_back(*d);
    return v;
}

char TerrainChar(Terrain t) {
    switch (t) {
        case Terrain::Wall: return '#';
        case Terrain::Difficult: return '~';
        case Terrain::HalfCover: return 'c';
        case Terrain::ThreeQuarterCover: return 'O';
        default: return '.';
    }
}
Terrain TerrainFrom(char c) {
    switch (c) {
        case '#': return Terrain::Wall;
        case '~': return Terrain::Difficult;
        case 'c': return Terrain::HalfCover;
        case 'O': return Terrain::ThreeQuarterCover;
        default: return Terrain::Floor;
    }
}

Json GridRows(const TacticalGrid& g) {
    Json rows = Json::array();
    for (int y = 0; y < g.Height(); ++y) {
        std::string r;
        for (int x = 0; x < g.Width(); ++x) r.push_back(TerrainChar(g.At({x, y})));
        rows.push_back(r);
    }
    return rows;
}

Json AttackJson(const WeaponAttack& w) {
    return Json{{"name", w.name}, {"toHit", w.toHit}, {"damage", w.damage.ToString()}, {"type", std::string(ToString(w.type))},
                {"rangeFt", w.rangeFt}, {"longRangeFt", w.longRangeFt}};
}

}  // namespace

Json ToJson(const DiceExpr& d) { return Json{{"count", d.count}, {"sides", d.sides}, {"bonus", d.bonus}}; }
DiceExpr DiceFromJson(const Json& j) { return DiceExpr{j.value("count", 0), j.value("sides", 0), j.value("bonus", 0)}; }

Json ToJson(const Effect& e) {
    return Json{{"id", e.id}, {"name", e.name}, {"condition", CondName(e.condition)}, {"rounds", e.rounds}, {"sourceId", e.sourceId},
                {"concentration", e.concentration}, {"saveEnds", e.saveEnds}, {"saveAbility", std::string(ToString(e.saveAbility))},
                {"saveDc", e.saveDc}, {"acBonus", e.acBonus}, {"meleeDamageBonus", e.meleeDamageBonus}, {"resistPhysical", e.resistPhysical},
                {"advantageAgainst", e.advantageAgainst}, {"expireAtSourceTurn", e.expireAtSourceTurn}};
}

Effect EffectFromJson(const Json& j) {
    Effect e;
    e.id = j.value("id", "");
    e.name = j.value("name", "");
    e.condition = CondFrom(j.value("condition", ""));
    e.rounds = j.value("rounds", -1);
    e.sourceId = j.value("sourceId", -1);
    e.concentration = j.value("concentration", false);
    e.saveEnds = j.value("saveEnds", false);
    e.saveAbility = ParseAbility(j.value("saveAbility", "wis")).value_or(Ability::Wis);
    e.saveDc = j.value("saveDc", 10);
    e.acBonus = j.value("acBonus", 0);
    e.meleeDamageBonus = j.value("meleeDamageBonus", 0);
    e.resistPhysical = j.value("resistPhysical", false);
    e.advantageAgainst = j.value("advantageAgainst", false);
    e.expireAtSourceTurn = j.value("expireAtSourceTurn", false);
    return e;
}

Json ToJson(const CharacterSheet& s) {
    Json j;
    j["name"] = s.name;
    j["classId"] = s.classId;
    j["tags"] = s.tags;
    j["monsterId"] = s.monsterId;
    j["level"] = s.level;
    j["scores"] = s.scores;
    j["saveProficient"] = s.saveProficient;
    j["proficiency"] = s.proficiency;
    j["armorClass"] = s.armorClass;
    j["speedFt"] = s.speedFt;
    j["maxHp"] = s.maxHp;
    j["hp"] = s.hp;
    j["tempHp"] = s.tempHp;
    j["hitDie"] = s.hitDie;
    j["hitDiceLeft"] = s.hitDiceLeft;
    j["exhaustion"] = s.exhaustion;
    j["spellAbility"] = std::string(ToString(s.spellAbility));
    j["slotsMax"] = s.slotsMax;
    j["slots"] = s.slots;
    j["spells"] = s.spells;
    Json atk = Json::array();
    for (const auto& w : s.attacks) {
        Json a = AttackJson(w);
        a["finesse"] = w.finesse;
        a["magical"] = w.magical;
        if (w.rider.present) {
            Json r{{"save", std::string(ToString(w.rider.save))}, {"dc", w.rider.dc}, {"condition", CondName(w.rider.condition)},
                   {"rounds", w.rider.rounds}, {"saveEnds", w.rider.saveEnds}, {"exceptTags", w.rider.exceptTags}};
            a["rider"] = r;
        }
        a["damageDice"] = ToJson(w.damage);
        atk.push_back(a);
    }
    j["attacks"] = atk;
    j["attacksPerAction"] = s.attacksPerAction;
    j["potions"] = s.potions;
    j["sneakAttackDice"] = s.sneakAttackDice;
    j["xpValue"] = s.xpValue;
    j["resist"] = Damages(s.resist);
    j["vulnerable"] = Damages(s.vulnerable);
    j["immune"] = Damages(s.immune);
    Json ci = Json::array();
    for (auto c : s.conditionImmune) ci.push_back(CondName(c));
    j["conditionImmune"] = ci;
    j["resistNonmagicalPhysical"] = s.resistNonmagicalPhysical;
    j["traits"] = s.traits;
    j["featureUses"] = s.featureUses;
    j["featureMax"] = s.featureMax;
    j["economy"] = Json{{"action", s.economy.action}, {"bonusAction", s.economy.bonusAction}, {"reaction", s.economy.reaction},
                        {"movementFt", s.economy.movementFt}, {"attacksLeft", s.economy.attacksLeft}};
    return j;
}

CharacterSheet SheetFromJson(const Json& j) {
    CharacterSheet s;
    s.name = j.value("name", "");
    s.classId = j.value("classId", "");
    s.tags = j.value("tags", std::vector<std::string>{});
    s.monsterId = j.value("monsterId", "");
    s.level = j.value("level", 1);
    s.scores = j.value("scores", s.scores);
    s.saveProficient = j.value("saveProficient", s.saveProficient);
    s.proficiency = j.value("proficiency", 2);
    s.armorClass = j.value("armorClass", 10);
    s.speedFt = j.value("speedFt", 30);
    s.maxHp = j.value("maxHp", 1);
    s.hp = j.value("hp", s.maxHp);
    s.tempHp = j.value("tempHp", 0);
    s.hitDie = j.value("hitDie", 8);
    s.hitDiceLeft = j.value("hitDiceLeft", 1);
    s.exhaustion = j.value("exhaustion", 0);
    s.spellAbility = ParseAbility(j.value("spellAbility", "int")).value_or(Ability::Int);
    s.slotsMax = j.value("slotsMax", s.slotsMax);
    s.slots = j.value("slots", s.slots);
    s.spells = j.value("spells", std::vector<std::string>{});
    if (j.contains("attacks"))
        for (const auto& a : j.at("attacks")) {
            WeaponAttack w;
            w.name = a.value("name", "");
            w.toHit = a.value("toHit", 0);
            w.damage = a.contains("damageDice") ? DiceFromJson(a.at("damageDice")) : DiceExpr{};
            w.type = ParseDamageType(a.value("type", "slashing")).value_or(DamageType::Slashing);
            w.rangeFt = a.value("rangeFt", 5);
            w.longRangeFt = a.value("longRangeFt", 0);
            w.finesse = a.value("finesse", false);
            w.magical = a.value("magical", false);
            if (a.contains("rider")) {
                const auto& r = a.at("rider");
                w.rider.present = true;
                w.rider.save = ParseAbility(r.value("save", "con")).value_or(Ability::Con);
                w.rider.dc = r.value("dc", 10);
                w.rider.condition = CondFrom(r.value("condition", ""));
                w.rider.rounds = r.value("rounds", 10);
                w.rider.saveEnds = r.value("saveEnds", false);
                w.rider.exceptTags = r.value("exceptTags", std::vector<std::string>{});
            }
            s.attacks.push_back(std::move(w));
        }
    s.attacksPerAction = j.value("attacksPerAction", 1);
    s.potions = j.value("potions", 0);
    s.sneakAttackDice = j.value("sneakAttackDice", 0);
    s.xpValue = j.value("xpValue", 0);
    if (j.contains("resist")) s.resist = DamagesFrom(j.at("resist"));
    if (j.contains("vulnerable")) s.vulnerable = DamagesFrom(j.at("vulnerable"));
    if (j.contains("immune")) s.immune = DamagesFrom(j.at("immune"));
    if (j.contains("conditionImmune"))
        for (const auto& c : j.at("conditionImmune")) {
            const Condition cc = CondFrom(c.get<std::string>());
            if (cc != Condition::None) s.conditionImmune.push_back(cc);
        }
    s.resistNonmagicalPhysical = j.value("resistNonmagicalPhysical", false);
    s.traits = j.value("traits", std::map<std::string, int>{});
    s.featureUses = j.value("featureUses", std::map<std::string, int>{});
    s.featureMax = j.value("featureMax", std::map<std::string, int>{});
    if (j.contains("economy")) {
        const auto& e = j.at("economy");
        s.economy.action = e.value("action", true);
        s.economy.bonusAction = e.value("bonusAction", true);
        s.economy.reaction = e.value("reaction", true);
        s.economy.movementFt = e.value("movementFt", 0);
        s.economy.attacksLeft = e.value("attacksLeft", 0);
    }
    return s;
}

namespace {
const char* TypeNames[] = {"move", "attack", "cast", "dash", "dodge", "potion", "feature", "swap_role", "end_turn", "await"};
}

namespace {
const char* const kEventNames[] = {"combat_start", "round_start", "turn_start", "move", "attack", "spell", "heal", "down", "death",
                                   "death_save", "info", "combat_end", "narration", "scene", "quest", "rest", "travel"};
}

Json ToJson(const Event& e) {
    return Json{{"seq", e.seq}, {"type", kEventNames[static_cast<int>(e.type)]}, {"actor", e.actor}, {"target", e.target}, {"roll", e.roll},
                {"vs", e.vs}, {"amount", e.amount}, {"text", e.text}, {"flavour", e.flavour}};
}

Event EventFromJson(const Json& j) {
    Event e;
    const std::string t = j.value("type", "info");
    for (int i = 0; i < 17; ++i)
        if (t == kEventNames[i]) e.type = static_cast<EventType>(i);
    e.seq = j.value("seq", 0);
    e.actor = j.value("actor", -1);
    e.target = j.value("target", -1);
    e.roll = j.value("roll", 0);
    e.vs = j.value("vs", 0);
    e.amount = j.value("amount", 0);
    e.text = j.value("text", "");
    e.flavour = j.value("flavour", "");
    return e;
}

Json ToJson(const Action& a) {
    Json j{{"type", TypeNames[static_cast<int>(a.type)]}};
    if (a.targetId >= 0) j["target"] = a.targetId;
    if (a.type == ActionType::Move) j["dest"] = {a.dest.x, a.dest.y};
    if (a.attackIndex >= 0) j["attack"] = a.attackIndex;
    if (!a.spellId.empty()) j["spell"] = a.spellId;
    if (!a.featureId.empty()) j["feature"] = a.featureId;
    if (a.slotLevel) j["slot"] = a.slotLevel;
    if (a.type == ActionType::SwapRole) j["role"] = std::string(ToString(a.role));
    return j;
}

bool ActionFromJson(const Json& j, Action& out, std::string& err) {
    out = Action{};
    if (!j.is_object() || !j.contains("type") || !j.at("type").is_string()) { err = "action needs a string \"type\""; return false; }
    const std::string t = j.at("type").get<std::string>();
    int found = -1;
    for (int i = 0; i < 10; ++i)
        if (t == TypeNames[i]) found = i;
    if (found < 0 || found == static_cast<int>(ActionType::AwaitInput)) { err = "unknown action type '" + t + "'"; return false; }
    out.type = static_cast<ActionType>(found);
    try {
        out.targetId = j.value("target", -1);
        out.attackIndex = j.value("attack", -1);
        out.spellId = j.value("spell", "");
        out.featureId = j.value("feature", "");
        out.slotLevel = j.value("slot", 0);
        if (out.type == ActionType::Move) {
            if (!j.contains("dest") || !j.at("dest").is_array() || j.at("dest").size() != 2) { err = "move needs \"dest\": [x, y]"; return false; }
            out.dest = Pos{j.at("dest")[0].get<int>(), j.at("dest")[1].get<int>()};
        }
        if (out.type == ActionType::SwapRole) {
            auto r = ParseRole(j.value("role", ""));
            if (!r) { err = "swap_role needs \"role\": player|dm|spectator"; return false; }
            out.role = *r;
        }
    } catch (const Json::exception& e) {
        err = std::string("malformed action: ") + e.what();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- Combat persistence

Json Combat::Save() const {
    Json j;
    Json g{{"width", grid_.Width()}, {"height", grid_.Height()}, {"rows", GridRows(grid_)}};
    j["grid"] = g;
    Json actors = Json::array();
    for (const auto& a : actors_) {
        Json x;
        x["id"] = a->id;
        x["team"] = a->team == Team::Party ? "party" : "monsters";
        x["sheet"] = ToJson(a->sheet);
        x["pos"] = {a->pos.x, a->pos.y};
        x["life"] = static_cast<int>(a->life);
        x["deathSaveSuccesses"] = a->deathSaveSuccesses;
        x["deathSaveFailures"] = a->deathSaveFailures;
        x["dodging"] = a->dodging;
        x["sneakUsedThisTurn"] = a->sneakUsedThisTurn;
        x["dashedThisTurn"] = a->dashedThisTurn;
        Json fx = Json::array();
        for (const auto& e : a->effects) fx.push_back(ToJson(e));
        x["effects"] = fx;
        x["concentrating"] = a->concentrating;
        x["controller"] = a->controller ? std::string(a->controller->Kind()) : std::string();
        actors.push_back(x);
    }
    j["actors"] = actors;
    j["order"] = order_;
    j["idx"] = idx_;
    j["round"] = round_;
    j["maxRounds"] = maxRounds_;
    j["started"] = started_;
    j["finished"] = finished_;
    j["inTurn"] = inTurn_;
    j["invalid"] = invalid_;
    j["steps"] = steps_;
    j["awaiting"] = awaiting_;
    j["result"] = Json{{"outcome", static_cast<int>(result_.outcome)}, {"rounds", result_.rounds},
                       {"xpPerSurvivor", result_.xpPerSurvivor}, {"xpTotal", result_.xpTotal}};
    return j;
}

void Combat::Load(const Json& j) {
    const auto& g = j.at("grid");
    grid_ = TacticalGrid(g.at("width").get<int>(), g.at("height").get<int>());
    const auto& rows = g.at("rows");
    for (int y = 0; y < grid_.Height(); ++y) {
        const std::string r = rows.at(static_cast<std::size_t>(y)).get<std::string>();
        for (int x = 0; x < grid_.Width() && x < static_cast<int>(r.size()); ++x) grid_.Set({x, y}, TerrainFrom(r[static_cast<std::size_t>(x)]));
    }
    actors_.clear();
    for (const auto& x : j.at("actors")) {
        auto a = std::make_unique<GameActor>();
        a->id = x.at("id").get<int>();
        a->team = x.at("team").get<std::string>() == "party" ? Team::Party : Team::Monsters;
        a->sheet = SheetFromJson(x.at("sheet"));
        a->pos = Pos{x.at("pos")[0].get<int>(), x.at("pos")[1].get<int>()};
        a->life = static_cast<LifeState>(x.at("life").get<int>());
        a->deathSaveSuccesses = x.value("deathSaveSuccesses", 0);
        a->deathSaveFailures = x.value("deathSaveFailures", 0);
        a->dodging = x.value("dodging", false);
        a->sneakUsedThisTurn = x.value("sneakUsedThisTurn", false);
        a->dashedThisTurn = x.value("dashedThisTurn", false);
        if (x.contains("effects"))
            for (const auto& e : x.at("effects")) a->effects.push_back(EffectFromJson(e));
        a->concentrating = x.value("concentrating", "");
        if (a->life != LifeState::Dead) grid_.SetOccupant(a->pos, a->id);
        actors_.push_back(std::move(a));
    }
    order_ = j.value("order", std::vector<int>{});
    idx_ = j.value("idx", std::size_t{0});
    round_ = j.value("round", 0);
    maxRounds_ = j.value("maxRounds", 30);
    started_ = j.value("started", false);
    finished_ = j.value("finished", false);
    inTurn_ = j.value("inTurn", false);
    invalid_ = j.value("invalid", 0);
    steps_ = j.value("steps", 0);
    awaiting_ = j.value("awaiting", -1);
    if (j.contains("result")) {
        const auto& r = j.at("result");
        result_.outcome = static_cast<Outcome>(r.value("outcome", 2));
        result_.rounds = r.value("rounds", 0);
        result_.xpPerSurvivor = r.value("xpPerSurvivor", 0);
        result_.xpTotal = r.value("xpTotal", 0);
    }
}

// ---------------------------------------------------------------- view

Json CombatView(const Combat& c) {
    Json v;
    v["round"] = c.Round();
    v["finished"] = c.IsFinished();
    v["awaiting"] = c.AwaitingActor();
    v["current"] = c.CurrentActorId();
    v["grid"] = Json{{"width", c.Grid().Width()}, {"height", c.Grid().Height()}, {"rows", GridRows(c.Grid())}};
    v["order"] = c.Order();
    Json actors = Json::array();
    static const char* life[] = {"alive", "unconscious", "stable", "dead"};
    for (const auto& a : c.Actors()) {
        Json x{{"id", a->id}, {"name", a->Name()}, {"team", a->team == Team::Party ? "party" : "monsters"}, {"glyph", std::string(1, c.Glyph(*a))}, {"classId", a->sheet.classId}, {"monsterId", a->sheet.monsterId}, {"tags", a->sheet.tags},
               {"pos", {a->pos.x, a->pos.y}}, {"hp", a->sheet.hp}, {"maxHp", a->sheet.maxHp}, {"ac", a->EffectiveAc()},
               {"life", life[static_cast<int>(a->life)]}, {"concentrating", a->concentrating},
               {"controller", a->controller ? std::string(a->controller->Kind()) : std::string()}};
        Json fx = Json::array();
        for (const auto& e : a->effects) fx.push_back(e.name.empty() ? e.id : e.name);
        x["effects"] = fx;
        actors.push_back(x);
    }
    v["actors"] = actors;

    const int wait = c.AwaitingActor();
    if (wait >= 0) {
        const GameActor* me = c.Find(wait);
        Json opts;
        const auto& s = me->sheet;
        opts["actor"] = wait;
        opts["economy"] = ToJson(s).at("economy");
        Json atk = Json::array();
        for (std::size_t i = 0; i < s.attacks.size(); ++i) {
            Json a = AttackJson(s.attacks[i]);
            a["index"] = i;
            atk.push_back(a);
        }
        opts["attacks"] = atk;
        Json sp = Json::array();
        for (const auto& id : s.spells)
            if (const Spell* x = Content::FindSpell(id)) {
                const bool ok = x->level == 0 || s.LowestSlotAtLeast(x->level) > 0;
                sp.push_back(Json{{"id", x->id}, {"name", x->name}, {"level", x->level}, {"rangeFt", x->rangeFt}, {"bonusAction", x->bonusAction},
                                  {"concentration", x->concentration}, {"available", ok}});
            }
        opts["spells"] = sp;
        Json ft = Json::array();
        for (const auto& [id, n] : s.featureUses) ft.push_back(Json{{"id", id}, {"uses", n}});
        opts["features"] = ft;
        opts["potions"] = s.potions;
        opts["slots"] = s.slots;
        v["options"] = opts;
    }
    if (c.IsFinished()) {
        const auto& r = c.Result();
        static const char* o[] = {"victory", "defeat", "timeout"};
        v["result"] = Json{{"outcome", o[static_cast<int>(r.outcome)]}, {"rounds", r.rounds}, {"xpPerSurvivor", r.xpPerSurvivor}, {"xpTotal", r.xpTotal}};
    }
    return v;
}

}  // namespace dnd
