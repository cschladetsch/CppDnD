#pragma once
#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dnd/Types.hpp"

namespace dnd {

inline const DiceExpr kPotionOfHealing{2, 4, 2};

// ---------------------------------------------------------------- rules content

struct ClassDef {
    std::string id, name;
    int hitDie = 8;
    std::vector<Ability> saves;
    std::string caster = "none";  // none | full | half
    Ability spellAbility = Ability::Int;
    std::vector<int> extraAttackLevels;  // each level reached adds one attack per Attack action
    bool sneakAttack = false;
    std::vector<std::pair<std::string, int>> features;  // id, level gained
};

struct WeaponDef {
    std::string id;
    WeaponAttack attack;  // toHit and damage bonus are filled in when a character is built
    std::string ability;  // "", "str" or "dex"; empty = dex for ranged, best of str/dex for finesse, else str
};

struct CharacterDef {
    std::string id, name, classId;
    int level = 3;
    std::array<int, 6> scores{10, 10, 10, 10, 10, 10};
    int ac = 10;
    int speed = 30;
    std::vector<std::string> tags, weapons, spells;
    int potions = 0;
};

struct MonsterDef {
    std::string id, name, cr;
    int xp = 0;
    CharacterSheet sheet;  // template; name is overwritten per instance
};

// ---------------------------------------------------------------- module content

// A "module" describes the space of a long-running session: a graph of locations, a clock,
// flags, quests, NPCs, random encounter tables and scripted events (cf. a gothic domain such as Ravenloft).
struct EncounterDef {
    std::vector<std::string> monsters;  // explicit list, or...
    std::string difficulty;             // ...generated for the current party: easy|medium|hard|deadly
};

struct EventDef {
    std::string id;
    std::string trigger = "enter";  // enter | search
    std::vector<std::string> needs;  // "flag", "!flag", "flag=value", "!flag=value"
    bool once = true;
    std::optional<EncounterDef> encounter;
    std::string narration;
    std::vector<std::string> setsFlags;   // applied when the event resolves (after a won encounter)
    int potions = 0;
    int xp = 0;
    std::vector<std::string> map;         // optional tactical map override
};

struct TopicDef {
    std::string id, text;
    std::vector<std::string> needs, setsFlags;
    int potions = 0;
};

struct NpcDef {
    std::string id, name, description;
    std::string type = "commoner";  // classifies the NPC; agents.json can pick a model per type
    std::vector<TopicDef> topics;
};

struct ExitDef {
    std::string to, label;
    double miles = 0;
    double hours = 0;  // overrides miles/pace when > 0
    std::vector<std::string> needs;
    std::string blockedText;
    std::string table;  // random table override while travelling this exit
};

struct LocationDef {
    std::string id, name, kind = "site", description;
    std::string rest = "risky";  // safe | risky | none
    std::string randomTable;
    std::vector<std::string> map;
    std::vector<ExitDef> exits;
    std::vector<EventDef> events;
    std::vector<NpcDef> npcs;
};

struct RandomEntry {
    int weight = 1;
    std::vector<std::string> monsters;
};

struct RandomTableDef {
    std::string id;
    int chanceDay = 5;    // percent per hour
    int chanceNight = 15;
    std::vector<RandomEntry> entries;
};

struct QuestDef {
    std::string id, name, description;
    std::vector<std::string> completeWhen;
    int xp = 0;
};

struct DrawDef {
    std::string id;
    std::vector<std::string> choices;  // one is drawn at game start and stored as flag "<id>=<choice>"
};

struct ModuleDef {
    std::string id, name, setting, intro, endText;
    int recommendedLevel = 3;
    double hardHp = 1.25;  // monster hit point scale on Hard (tuned per module so an AI-only party wins about one game in three)
    int startHour = 18;
    int nightStart = 19, nightEnd = 6;
    std::string start;
    std::vector<DrawDef> draws;
    std::vector<RandomTableDef> tables;
    std::vector<QuestDef> quests;
    std::vector<LocationDef> locations;
    std::vector<std::string> endWhen;

    const LocationDef* FindLocation(std::string_view id) const;
    const RandomTableDef* FindTable(std::string_view id) const;
    bool IsNight(int absoluteHour) const;
};

namespace Content {

// Loads every JSON file under `dir` (classes, weapons, spells, monsters, characters, modules/*.json).
// Throws std::runtime_error naming the file and field on any problem. Call once at startup.
void Load(const std::string& dir);
bool IsLoaded();
std::string DefaultDataDir();  // $DND_DATA, else the directory baked in at build time

const Spell* FindSpell(std::string_view id);
const std::vector<Spell>& Spells();
const std::vector<MonsterDef>& Monsters();
const MonsterDef* FindMonster(std::string_view id);
const ClassDef* FindClass(std::string_view id);
const std::vector<ClassDef>& Classes();
const std::vector<CharacterDef>& Characters();
const CharacterDef* FindCharacter(std::string_view id);
const std::vector<ModuleDef>& Modules();
const ModuleDef* FindModule(std::string_view id);

// Class tables applied to a character definition at the requested level (0 = the definition's own level).
CharacterSheet BuildCharacter(const CharacterDef& def, int level = 0);

int XpForLevel(int level);  // SRD cumulative XP to reach `level`
int LevelForXp(int xp);

// A linear dungeon of `rooms` generated encounters, for quick play without authoring a module.
ModuleDef MakeProceduralModule(int rooms, const std::string& difficulty);
const std::vector<std::string>& DefaultRoomMap();

}  // namespace Content
}  // namespace dnd
