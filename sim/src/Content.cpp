#include "dnd/Content.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "dnd/Util.hpp"

#ifndef DND_DATA_DIR
#define DND_DATA_DIR "data"
#endif

namespace dnd {
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct Library {
    std::vector<Spell> spells;
    std::vector<ClassDef> classes;
    std::vector<WeaponDef> weapons;
    std::vector<MonsterDef> monsters;
    std::vector<CharacterDef> characters;
    std::vector<ModuleDef> modules;
    std::map<std::string, std::vector<std::array<int, 9>>> slotTables;
    bool loaded = false;
};

Library& Lib() {
    static Library lib;
    return lib;
}

[[noreturn]] void Bad(const std::string& where, const std::string& msg) { throw std::runtime_error(where + ": " + msg); }

json ReadFile(const fs::path& p) {
    std::ifstream in(p);
    if (!in) Bad(p.string(), "cannot open file");
    try {
        return json::parse(in, nullptr, true, true);  // allow /* comments */
    } catch (const std::exception& e) {
        Bad(p.string(), e.what());
    }
}

DiceExpr Dice(const std::string& s, const std::string& where) {
    auto d = DiceExpr::Parse(s);
    if (!d) Bad(where, "bad dice expression '" + s + "'");
    return *d;
}

Ability Abil(const std::string& s, const std::string& where) {
    auto a = ParseAbility(s);
    if (!a) Bad(where, "unknown ability '" + s + "'");
    return *a;
}

DamageType Dmg(const std::string& s, const std::string& where) {
    auto d = ParseDamageType(s);
    if (!d) Bad(where, "unknown damage type '" + s + "'");
    return *d;
}

Condition Cond(const std::string& s, const std::string& where) {
    auto c = ParseCondition(s);
    if (!c) Bad(where, "unknown condition '" + s + "'");
    return *c;
}

std::vector<std::string> Strs(const json& j, const char* key) {
    std::vector<std::string> v;
    if (!j.contains(key)) return v;
    const auto& x = j.at(key);
    if (x.is_string()) v.push_back(x.get<std::string>());
    else for (const auto& e : x) v.push_back(e.get<std::string>());
    return v;
}

WeaponAttack ParseAttack(const json& j, const std::string& where) {
    WeaponAttack w;
    w.name = j.at("name").get<std::string>();
    w.toHit = j.value("toHit", 0);
    w.damage = Dice(j.at("damage").get<std::string>(), where);
    w.type = Dmg(j.value("type", "slashing"), where);
    w.finesse = j.value("finesse", false);
    w.magical = j.value("magical", false);
    if (j.contains("range")) {
        w.rangeFt = j.at("range").at(0).get<int>();
        w.longRangeFt = j.at("range").at(1).get<int>();
    } else {
        w.rangeFt = j.value("reach", 5);
    }
    if (j.contains("rider")) {
        const auto& r = j.at("rider");
        w.rider.present = true;
        w.rider.save = Abil(r.at("save").get<std::string>(), where);
        w.rider.dc = r.at("dc").get<int>();
        w.rider.condition = Cond(r.at("condition").get<std::string>(), where);
        w.rider.rounds = r.value("rounds", 10);
        w.rider.saveEnds = r.value("saveEnds", false);
        w.rider.exceptTags = Strs(r, "exceptTags");
    }
    return w;
}

Spell ParseSpell(const json& j, const std::string& where) {
    Spell s;
    s.id = j.at("id").get<std::string>();
    s.name = j.value("name", s.id);
    s.level = j.value("level", 0);
    s.bonusAction = j.value("bonusAction", false);
    s.rangeFt = j.value("range", 60);
    const std::string kind = j.at("kind").get<std::string>();
    static const std::map<std::string, SpellKind> kinds{
        {"attack", SpellKind::AttackRoll}, {"autoHit", SpellKind::AutoHit}, {"saveNegates", SpellKind::SaveNegates},
        {"saveForHalf", SpellKind::SaveForHalf}, {"heal", SpellKind::Heal}, {"condition", SpellKind::Condition}, {"buff", SpellKind::Buff}};
    auto it = kinds.find(kind);
    if (it == kinds.end()) Bad(where, "unknown spell kind '" + kind + "'");
    s.kind = it->second;
    if (j.contains("dice")) s.dice = Dice(j.at("dice").get<std::string>(), where);
    s.damageType = Dmg(j.value("damageType", "force"), where);
    if (j.contains("save")) s.save = Abil(j.at("save").get<std::string>(), where);
    s.radiusFt = j.value("radius", 0);
    s.projectiles = j.value("projectiles", 1);
    s.upcastDice = j.value("upcastDice", 0);
    s.upcastProjectiles = j.value("upcastProjectiles", 0);
    s.addsCastingMod = j.value("addsCastingMod", false);
    s.concentration = j.value("concentration", false);
    if (j.contains("condition")) s.condition = Cond(j.at("condition").get<std::string>(), where);
    s.durationRounds = j.value("durationRounds", 10);
    s.saveEnds = j.value("saveEnds", false);
    s.acBonus = j.value("acBonus", 0);
    s.grantsAdvantage = j.value("grantsAdvantage", false);
    s.requiresTag = j.value("requiresTag", "");
    return s;
}

std::array<int, 6> Scores(const json& j, const std::string& where) {
    if (!j.is_array() || j.size() != 6) Bad(where, "scores must be an array of 6 numbers");
    std::array<int, 6> a{};
    for (std::size_t i = 0; i < 6; ++i) a[i] = j[i].get<int>();
    return a;
}

MonsterDef ParseMonster(const json& j) {
    const std::string id = j.at("id").get<std::string>();
    const std::string where = "monsters.json[" + id + "]";
    MonsterDef m;
    m.id = id;
    m.name = j.at("name").get<std::string>();
    m.cr = j.value("cr", "?");
    m.xp = j.at("xp").get<int>();
    CharacterSheet& s = m.sheet;
    s.name = m.name;
    s.level = j.value("level", 1);
    s.proficiency = j.value("proficiency", 2);
    s.armorClass = j.at("ac").get<int>();
    s.maxHp = s.hp = j.at("hp").get<int>();
    s.speedFt = j.value("speed", 30);
    s.scores = Scores(j.at("scores"), where);
    for (const auto& a : Strs(j, "saves")) s.saveProficient[static_cast<std::size_t>(Abil(a, where))] = true;
    for (const auto& a : j.at("attacks")) s.attacks.push_back(ParseAttack(a, where));
    s.attacksPerAction = j.value("attacksPerAction", 1);
    s.tags = Strs(j, "tags");
    for (const auto& d : Strs(j, "resist")) s.resist.push_back(Dmg(d, where));
    for (const auto& d : Strs(j, "vulnerable")) s.vulnerable.push_back(Dmg(d, where));
    for (const auto& d : Strs(j, "immune")) s.immune.push_back(Dmg(d, where));
    for (const auto& c : Strs(j, "conditionImmune")) s.conditionImmune.push_back(Cond(c, where));
    s.resistNonmagicalPhysical = j.value("resistNonmagical", false);
    if (j.contains("traits"))
        for (auto it = j.at("traits").begin(); it != j.at("traits").end(); ++it) s.traits[it.key()] = it.value().get<int>();
    s.spells = Strs(j, "spells");
    if (j.contains("spellAbility")) s.spellAbility = Abil(j.at("spellAbility").get<std::string>(), where);
    if (j.contains("slots"))
        for (auto it = j.at("slots").begin(); it != j.at("slots").end(); ++it) {
            const int lvl = std::stoi(it.key());
            if (lvl < 1 || lvl > 9) Bad(where, "slot level out of range");
            s.slotsMax[static_cast<std::size_t>(lvl)] = s.slots[static_cast<std::size_t>(lvl)] = it.value().get<int>();
        }
    s.potions = j.value("potions", 0);
    s.xpValue = m.xp;
    s.hitDie = j.value("hitDie", 8);
    s.hitDiceLeft = s.level;
    return m;
}

std::vector<EventDef> ParseEvents(const json& arr, const std::string& where) {
    std::vector<EventDef> out;
    for (const auto& j : arr) {
        EventDef e;
        e.id = j.at("id").get<std::string>();
        e.trigger = j.value("trigger", "enter");
        if (e.trigger != "enter" && e.trigger != "search") Bad(where + "/" + e.id, "trigger must be enter or search");
        e.needs = Strs(j, "requires");
        e.once = j.value("once", true);
        if (j.contains("encounter")) {
            EncounterDef enc;
            enc.monsters = Strs(j.at("encounter"), "monsters");
            enc.difficulty = j.at("encounter").value("difficulty", "");
            if (enc.monsters.empty() && enc.difficulty.empty()) Bad(where + "/" + e.id, "encounter needs monsters or difficulty");
            e.encounter = std::move(enc);
        }
        e.narration = j.value("narration", "");
        e.setsFlags = Strs(j, "setsFlags");
        e.potions = j.value("potions", 0);
        e.xp = j.value("xp", 0);
        e.map = Strs(j, "map");
        out.push_back(std::move(e));
    }
    return out;
}

ModuleDef ParseModule(const json& j, const std::string& file) {
    ModuleDef m;
    m.id = j.at("id").get<std::string>();
    const std::string where = file + "[" + m.id + "]";
    m.name = j.at("name").get<std::string>();
    m.setting = j.value("setting", "");
    m.intro = j.value("intro", "");
    m.endText = j.value("endText", "The tale is told.");
    m.recommendedLevel = j.value("recommendedLevel", 3);
    if (j.contains("clock")) {
        m.startHour = j.at("clock").value("startHour", 18);
        m.nightStart = j.at("clock").value("nightStart", 19);
        m.nightEnd = j.at("clock").value("nightEnd", 6);
    }
    m.start = j.at("start").get<std::string>();
    m.endWhen = Strs(j, "endWhen");
    if (j.contains("draws"))
        for (const auto& d : j.at("draws")) m.draws.push_back(DrawDef{d.at("id").get<std::string>(), Strs(d, "choices")});
    if (j.contains("tables"))
        for (const auto& t : j.at("tables")) {
            RandomTableDef rt;
            rt.id = t.at("id").get<std::string>();
            rt.chanceDay = t.value("chanceDay", 5);
            rt.chanceNight = t.value("chanceNight", 15);
            for (const auto& e : t.at("entries")) rt.entries.push_back(RandomEntry{e.value("weight", 1), Strs(e, "monsters")});
            m.tables.push_back(std::move(rt));
        }
    if (j.contains("quests"))
        for (const auto& q : j.at("quests"))
            m.quests.push_back(QuestDef{q.at("id").get<std::string>(), q.value("name", ""), q.value("description", ""), Strs(q, "completeWhen"), q.value("xp", 0)});
    for (const auto& l : j.at("locations")) {
        LocationDef loc;
        loc.id = l.at("id").get<std::string>();
        loc.name = l.value("name", loc.id);
        loc.kind = l.value("kind", "site");
        loc.description = l.value("description", "");
        loc.rest = l.value("rest", "risky");
        if (loc.rest != "safe" && loc.rest != "risky" && loc.rest != "none") Bad(where + "/" + loc.id, "rest must be safe|risky|none");
        loc.randomTable = l.value("randomTable", "");
        loc.map = Strs(l, "map");
        if (l.contains("exits"))
            for (const auto& x : l.at("exits")) {
                ExitDef e;
                e.to = x.at("to").get<std::string>();
                e.label = x.value("label", e.to);
                e.miles = x.value("miles", 0.0);
                e.hours = x.value("hours", 0.0);
                e.needs = Strs(x, "requires");
                e.blockedText = x.value("blockedText", "The way is barred.");
                e.table = x.value("table", "");
                loc.exits.push_back(std::move(e));
            }
        if (l.contains("events")) loc.events = ParseEvents(l.at("events"), where + "/" + loc.id);
        if (l.contains("npcs"))
            for (const auto& n : l.at("npcs")) {
                NpcDef npc;
                npc.id = n.at("id").get<std::string>();
                npc.name = n.value("name", npc.id);
                npc.description = n.value("description", "");
                if (n.contains("topics"))
                    for (const auto& t : n.at("topics"))
                        npc.topics.push_back(TopicDef{t.at("id").get<std::string>(), t.value("text", ""), Strs(t, "requires"), Strs(t, "setsFlags"), t.value("potions", 0)});
                loc.npcs.push_back(std::move(npc));
            }
        m.locations.push_back(std::move(loc));
    }
    return m;
}

// ---- cross-reference validation

std::vector<std::string> RowsValid(const std::vector<std::string>& map) {
    std::vector<std::string> errs;
    for (std::size_t y = 0; y < map.size(); ++y)
        for (char c : map[y])
            if (std::string(".#~cOPM").find(c) == std::string::npos) errs.push_back(Cat("row ", y, " has unknown map character '", std::string(1, c), "'"));
    return errs;
}

void ValidateModule(const ModuleDef& m, const Library& lib) {
    const std::string where = "module " + m.id;
    std::set<std::string> ids;
    for (const auto& l : m.locations) if (!ids.insert(l.id).second) Bad(where, "duplicate location id " + l.id);
    if (!ids.count(m.start)) Bad(where, "start location '" + m.start + "' does not exist");
    auto checkMonsters = [&](const std::vector<std::string>& v, const std::string& ctx) {
        for (const auto& id : v) {
            bool found = false;
            for (const auto& mon : lib.monsters) found = found || mon.id == id;
            if (!found) Bad(where + "/" + ctx, "unknown monster '" + id + "'");
        }
    };
    for (const auto& l : m.locations) {
        for (const auto& e : RowsValid(l.map)) Bad(where + "/" + l.id, e);
        for (const auto& x : l.exits) if (!ids.count(x.to)) Bad(where + "/" + l.id, "exit to unknown location '" + x.to + "'");
        if (!l.randomTable.empty() && !m.FindTable(l.randomTable)) Bad(where + "/" + l.id, "unknown random table '" + l.randomTable + "'");
        for (const auto& x : l.exits) if (!x.table.empty() && !m.FindTable(x.table)) Bad(where + "/" + l.id, "unknown random table '" + x.table + "'");
        for (const auto& e : l.events) {
            for (const auto& err : RowsValid(e.map)) Bad(where + "/" + l.id + "/" + e.id, err);
            if (e.encounter) {
                checkMonsters(e.encounter->monsters, l.id + "/" + e.id);
                if (!e.encounter->difficulty.empty() && !(e.encounter->difficulty == "easy" || e.encounter->difficulty == "medium" || e.encounter->difficulty == "hard" || e.encounter->difficulty == "deadly")) Bad(where + "/" + l.id + "/" + e.id, "bad difficulty");
            }
        }
    }
    for (const auto& t : m.tables) for (const auto& e : t.entries) checkMonsters(e.monsters, "table " + t.id);
}

}  // namespace

// ---------------------------------------------------------------- ModuleDef helpers

const LocationDef* ModuleDef::FindLocation(std::string_view id) const {
    for (const auto& l : locations)
        if (l.id == id) return &l;
    return nullptr;
}

const RandomTableDef* ModuleDef::FindTable(std::string_view id) const {
    for (const auto& t : tables)
        if (t.id == id) return &t;
    return nullptr;
}

bool ModuleDef::IsNight(int absoluteHour) const {
    const int h = ((absoluteHour % 24) + 24) % 24;
    return nightStart > nightEnd ? (h >= nightStart || h < nightEnd) : (h >= nightStart && h < nightEnd);
}

namespace Content {
namespace {

int FeatureUsesFor(const std::string& id, int level) {
    if (id == "rage") return level >= 20 ? 99 : level >= 17 ? 6 : level >= 12 ? 5 : level >= 6 ? 4 : level >= 3 ? 3 : 2;
    return 1;  // second_wind and anything else: one use per rest
}

}  // namespace

std::string DefaultDataDir() {
    if (const char* e = std::getenv("DND_DATA"); e && *e) return e;
    return DND_DATA_DIR;
}

bool IsLoaded() { return Lib().loaded; }

void Load(const std::string& dir) {
    Library lib;
    const fs::path d(dir);

    {   // classes
        const json j = ReadFile(d / "classes.json");
        for (auto it = j.at("spellSlotTables").begin(); it != j.at("spellSlotTables").end(); ++it) {
            std::vector<std::array<int, 9>> rows;
            for (const auto& row : it.value()) {
                std::array<int, 9> r{};
                for (std::size_t i = 0; i < row.size() && i < 9; ++i) r[i] = row[i].get<int>();
                rows.push_back(r);
            }
            if (rows.size() != 20) Bad("classes.json", "slot table '" + it.key() + "' needs 20 rows");
            lib.slotTables[it.key()] = std::move(rows);
        }
        for (const auto& c : j.at("classes")) {
            ClassDef cd;
            cd.id = c.at("id").get<std::string>();
            const std::string where = "classes.json[" + cd.id + "]";
            cd.name = c.value("name", cd.id);
            cd.hitDie = c.at("hitDie").get<int>();
            for (const auto& s : Strs(c, "saves")) cd.saves.push_back(Abil(s, where));
            cd.caster = c.value("caster", "none");
            if (cd.caster != "none" && !lib.slotTables.count(cd.caster)) Bad(where, "no slot table for caster type '" + cd.caster + "'");
            if (c.contains("spellAbility")) cd.spellAbility = Abil(c.at("spellAbility").get<std::string>(), where);
            if (c.contains("extraAttack")) for (const auto& l : c.at("extraAttack")) cd.extraAttackLevels.push_back(l.get<int>());
            cd.sneakAttack = c.value("sneakAttack", false);
            if (c.contains("features"))
                for (const auto& f : c.at("features")) cd.features.emplace_back(f.at("id").get<std::string>(), f.value("level", 1));
            lib.classes.push_back(std::move(cd));
        }
    }
    for (const auto& w : ReadFile(d / "weapons.json")) {
        WeaponDef wd;
        wd.id = w.at("id").get<std::string>();
        wd.attack = ParseAttack(w, "weapons.json[" + wd.id + "]");
        wd.ability = w.value("ability", "");
        lib.weapons.push_back(std::move(wd));
    }
    for (const auto& s : ReadFile(d / "spells.json")) lib.spells.push_back(ParseSpell(s, "spells.json[" + s.value("id", "?") + "]"));
    auto spellExists = [&](const std::string& id) {
        return std::any_of(lib.spells.begin(), lib.spells.end(), [&](const Spell& s) { return s.id == id; });
    };
    for (const auto& m : ReadFile(d / "monsters.json")) {
        lib.monsters.push_back(ParseMonster(m));
        for (const auto& sp : lib.monsters.back().sheet.spells)
            if (!spellExists(sp)) Bad("monsters.json[" + lib.monsters.back().id + "]", "unknown spell '" + sp + "'");
    }
    for (const auto& c : ReadFile(d / "characters.json")) {
        CharacterDef cd;
        cd.id = c.at("id").get<std::string>();
        const std::string where = "characters.json[" + cd.id + "]";
        cd.name = c.value("name", cd.id);
        cd.classId = c.at("class").get<std::string>();
        if (!std::any_of(lib.classes.begin(), lib.classes.end(), [&](const ClassDef& x) { return x.id == cd.classId; })) Bad(where, "unknown class '" + cd.classId + "'");
        cd.level = c.value("level", 3);
        cd.scores = Scores(c.at("scores"), where);
        cd.ac = c.at("ac").get<int>();
        cd.speed = c.value("speed", 30);
        cd.tags = Strs(c, "tags");
        cd.weapons = Strs(c, "weapons");
        cd.spells = Strs(c, "spells");
        cd.potions = c.value("potions", 0);
        for (const auto& w : cd.weapons)
            if (!std::any_of(lib.weapons.begin(), lib.weapons.end(), [&](const WeaponDef& x) { return x.id == w; })) Bad(where, "unknown weapon '" + w + "'");
        for (const auto& sp : cd.spells) if (!spellExists(sp)) Bad(where, "unknown spell '" + sp + "'");
        lib.characters.push_back(std::move(cd));
    }
    if (fs::exists(d / "modules")) {
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(d / "modules"))
            if (e.path().extension() == ".json") files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const auto& f : files) lib.modules.push_back(ParseModule(ReadFile(f), f.filename().string()));
    }
    for (const auto& m : lib.modules) ValidateModule(m, lib);

    lib.loaded = true;
    Lib() = std::move(lib);
}

const std::vector<Spell>& Spells() { return Lib().spells; }
const Spell* FindSpell(std::string_view id) {
    for (const auto& s : Lib().spells)
        if (s.id == id) return &s;
    return nullptr;
}
const std::vector<MonsterDef>& Monsters() { return Lib().monsters; }
const MonsterDef* FindMonster(std::string_view id) {
    for (const auto& m : Lib().monsters)
        if (m.id == id) return &m;
    return nullptr;
}
const std::vector<ClassDef>& Classes() { return Lib().classes; }
const ClassDef* FindClass(std::string_view id) {
    for (const auto& c : Lib().classes)
        if (c.id == id) return &c;
    return nullptr;
}
const std::vector<CharacterDef>& Characters() { return Lib().characters; }
const CharacterDef* FindCharacter(std::string_view id) {
    for (const auto& c : Lib().characters)
        if (c.id == id) return &c;
    return nullptr;
}
const std::vector<ModuleDef>& Modules() { return Lib().modules; }
const ModuleDef* FindModule(std::string_view id) {
    for (const auto& m : Lib().modules)
        if (m.id == id) return &m;
    return nullptr;
}

CharacterSheet BuildCharacter(const CharacterDef& d, int levelOverride) {
    const ClassDef* cls = FindClass(d.classId);
    if (!cls) throw std::runtime_error("unknown class " + d.classId);
    const int level = std::clamp(levelOverride > 0 ? levelOverride : d.level, 1, 20);

    CharacterSheet s;
    s.name = d.name;
    s.classId = d.classId;
    s.tags = d.tags;
    s.level = level;
    s.scores = d.scores;
    s.proficiency = 2 + (level - 1) / 4;  // SRD proficiency bonus
    s.armorClass = d.ac;
    s.speedFt = d.speed;
    const int con = s.Mod(Ability::Con);
    s.hitDie = cls->hitDie;
    s.maxHp = std::max(level, cls->hitDie + con + (level - 1) * (cls->hitDie / 2 + 1 + con));  // max at 1st, average after
    s.hp = s.maxHp;
    s.hitDiceLeft = level;
    for (Ability a : cls->saves) s.saveProficient[static_cast<std::size_t>(a)] = true;

    if (cls->caster != "none") {
        s.spellAbility = cls->spellAbility;
        const auto& row = Lib().slotTables.at(cls->caster)[static_cast<std::size_t>(level - 1)];
        for (std::size_t i = 0; i < 9; ++i) s.slotsMax[i + 1] = s.slots[i + 1] = row[i];
    }
    s.spells = d.spells;
    s.attacksPerAction = 1;
    for (int l : cls->extraAttackLevels)
        if (level >= l) ++s.attacksPerAction;
    if (cls->sneakAttack) s.sneakAttackDice = (level + 1) / 2;
    for (const auto& [id, from] : cls->features)
        if (level >= from) s.featureUses[id] = s.featureMax[id] = FeatureUsesFor(id, level);
    s.potions = d.potions;

    for (const auto& wid : d.weapons) {
        for (const auto& w : Lib().weapons) {
            if (w.id != wid) continue;
            WeaponAttack a = w.attack;
            int mod;
            if (w.ability == "str") mod = s.Mod(Ability::Str);
            else if (w.ability == "dex") mod = s.Mod(Ability::Dex);
            else if (a.IsRanged()) mod = s.Mod(Ability::Dex);
            else if (a.finesse) mod = std::max(s.Mod(Ability::Str), s.Mod(Ability::Dex));
            else mod = s.Mod(Ability::Str);
            a.toHit = s.proficiency + mod;
            a.damage.bonus += mod;
            s.attacks.push_back(std::move(a));
        }
    }
    return s;
}

int XpForLevel(int level) {
    static const int table[] = {0, 0, 300, 900, 2700, 6500, 14000, 23000, 34000, 48000, 64000, 85000, 100000,
                                120000, 140000, 165000, 195000, 225000, 265000, 305000, 355000};
    return table[std::clamp(level, 1, 20)];
}

int LevelForXp(int xp) {
    int level = 1;
    while (level < 20 && xp >= XpForLevel(level + 1)) ++level;
    return level;
}

const std::vector<std::string>& DefaultRoomMap() {
    static const std::vector<std::string> map{
        "....................", "....................", ".....~~.............", ".....~~..cc.........",
        "....................", "............O.......", "........O.....#.....", "..............#.....",
        ".........cc.........", ".............~~.....", ".............~~.....", "...................."};
    return map;
}

ModuleDef MakeProceduralModule(int rooms, const std::string& difficulty) {
    ModuleDef m;
    m.id = "procedural";
    m.name = "Delve";
    m.setting = "a stretch of dungeon corridors";
    m.intro = "Torchlight gutters as you descend into the dark.";
    m.endText = "The last chamber is silent. The delve is over.";
    rooms = std::max(1, rooms);
    m.start = "room1";
    for (int i = 1; i <= rooms; ++i) {
        LocationDef l;
        l.id = "room" + std::to_string(i);
        l.name = "Chamber " + std::to_string(i);
        l.kind = "site";
        l.description = "A cold stone chamber, scarred by old fights.";
        l.rest = "risky";
        l.map = DefaultRoomMap();
        EventDef e;
        e.id = "ambush";
        e.trigger = "enter";
        e.once = true;
        e.encounter = EncounterDef{{}, difficulty};
        e.setsFlags = {"room" + std::to_string(i) + "_clear"};
        l.events.push_back(std::move(e));
        if (i < rooms) {
            ExitDef x;
            x.to = "room" + std::to_string(i + 1);
            x.label = "Deeper into the dungeon";
            x.needs = {"room" + std::to_string(i) + "_clear"};
            x.blockedText = "Enemies still bar the way.";
            l.exits.push_back(std::move(x));
        }
        m.locations.push_back(std::move(l));
    }
    m.endWhen = {"room" + std::to_string(rooms) + "_clear"};
    return m;
}

}  // namespace Content
}  // namespace dnd
