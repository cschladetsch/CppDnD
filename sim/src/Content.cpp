#include "dnd/Content.hpp"

#include <array>

namespace dnd::Content {
namespace {

DiceExpr D(const char* s) { return *DiceExpr::Parse(s); }

WeaponAttack Melee(std::string name, int toHit, const char* dmg, DamageType t, bool finesse = false, int reach = 5) {
    WeaponAttack w;
    w.name = std::move(name); w.toHit = toHit; w.damage = D(dmg); w.type = t;
    w.rangeFt = reach; w.longRangeFt = 0; w.finesse = finesse;
    return w;
}

WeaponAttack Ranged(std::string name, int toHit, const char* dmg, DamageType t, int normal, int longRange) {
    WeaponAttack w;
    w.name = std::move(name); w.toHit = toHit; w.damage = D(dmg); w.type = t;
    w.rangeFt = normal; w.longRangeFt = longRange;
    return w;
}

CharacterSheet Base(std::string name, int level, std::array<int, 6> scores, int ac, int speed, int hp) {
    CharacterSheet s;
    s.name = std::move(name); s.level = level; s.scores = scores;
    s.armorClass = ac; s.speedFt = speed; s.maxHp = s.hp = hp;
    s.proficiency = 2 + (level - 1) / 4;
    return s;
}

std::vector<Spell> BuildSpells() {
    std::vector<Spell> v;
    auto add = [&](Spell s) { v.push_back(std::move(s)); };
    {   Spell s; s.id = "fire_bolt"; s.name = "Fire Bolt"; s.level = 0; s.rangeFt = 120;
        s.kind = SpellKind::AttackRoll; s.dice = D("1d10"); s.damageType = DamageType::Fire; add(s); }
    {   Spell s; s.id = "sacred_flame"; s.name = "Sacred Flame"; s.level = 0; s.rangeFt = 60;
        s.kind = SpellKind::SaveNegates; s.save = Ability::Dex; s.dice = D("1d8"); s.damageType = DamageType::Radiant; add(s); }
    {   Spell s; s.id = "magic_missile"; s.name = "Magic Missile"; s.level = 1; s.rangeFt = 120;
        s.kind = SpellKind::AutoHit; s.dice = D("1d4+1"); s.damageType = DamageType::Force;
        s.projectiles = 3; s.upcastProjectiles = 1; add(s); }
    {   Spell s; s.id = "shatter"; s.name = "Shatter"; s.level = 2; s.rangeFt = 60;
        s.kind = SpellKind::SaveForHalf; s.save = Ability::Con; s.dice = D("3d8"); s.damageType = DamageType::Thunder;
        s.radiusFt = 10; s.upcastDice = 1; add(s); }
    {   Spell s; s.id = "cure_wounds"; s.name = "Cure Wounds"; s.level = 1; s.rangeFt = 5;
        s.kind = SpellKind::Heal; s.dice = D("1d8"); s.upcastDice = 1; s.addsCastingMod = true; add(s); }
    {   Spell s; s.id = "healing_word"; s.name = "Healing Word"; s.level = 1; s.rangeFt = 60; s.bonusAction = true;
        s.kind = SpellKind::Heal; s.dice = D("1d4"); s.upcastDice = 1; s.addsCastingMod = true; add(s); }
    return v;
}

std::vector<MonsterDef> BuildMonsters() {
    std::vector<MonsterDef> v;
    auto add = [&](const char* id, const char* name, const char* cr, int xp, CharacterSheet s) {
        s.xpValue = xp;
        v.push_back(MonsterDef{id, name, cr, xp, std::move(s)});
    };
    using DT = DamageType;
    {   auto s = Base("Cultist", 1, {11, 12, 10, 10, 11, 10}, 12, 30, 9);
        s.attacks = {Melee("scimitar", 3, "1d6+1", DT::Slashing, true)};
        add("cultist", "Cultist", "1/8", 25, s); }
    {   auto s = Base("Goblin", 1, {8, 14, 10, 10, 8, 8}, 15, 30, 7);
        s.attacks = {Melee("scimitar", 4, "1d6+2", DT::Slashing, true), Ranged("shortbow", 4, "1d6+2", DT::Piercing, 80, 320)};
        add("goblin", "Goblin", "1/4", 50, s); }
    {   auto s = Base("Skeleton", 1, {10, 14, 15, 6, 8, 5}, 13, 30, 13);
        s.attacks = {Melee("shortsword", 4, "1d6+2", DT::Piercing, true), Ranged("shortbow", 4, "1d6+2", DT::Piercing, 80, 320)};
        add("skeleton", "Skeleton", "1/4", 50, s); }
    {   auto s = Base("Zombie", 1, {13, 6, 16, 3, 6, 5}, 8, 20, 22);
        s.attacks = {Melee("slam", 3, "1d6+1", DT::Bludgeoning)};
        add("zombie", "Zombie", "1/4", 50, s); }
    {   auto s = Base("Orc", 1, {16, 12, 16, 7, 11, 10}, 13, 30, 15);
        s.attacks = {Melee("greataxe", 5, "1d12+3", DT::Slashing), Ranged("javelin", 5, "1d6+3", DT::Piercing, 30, 120)};
        add("orc", "Orc", "1/2", 100, s); }
    {   auto s = Base("Bugbear", 1, {15, 14, 13, 8, 11, 9}, 16, 30, 27);
        s.attacks = {Melee("morningstar", 4, "2d8+2", DT::Piercing, false, 10), Ranged("javelin", 4, "2d6+2", DT::Piercing, 30, 120)};
        add("bugbear", "Bugbear", "1", 200, s); }
    {   auto s = Base("Ogre", 1, {19, 8, 16, 5, 7, 7}, 11, 40, 59);
        s.attacks = {Melee("greatclub", 6, "2d8+4", DT::Bludgeoning, false, 5), Ranged("javelin", 6, "2d6+4", DT::Piercing, 30, 120)};
        add("ogre", "Ogre", "2", 450, s); }
    return v;
}

void SetSaves(CharacterSheet& s, std::initializer_list<Ability> abilities) {
    for (auto a : abilities) s.saveProficient[static_cast<std::size_t>(a)] = true;
}

}  // namespace

const Spell* FindSpell(std::string_view id) {
    static const std::vector<Spell> spells = BuildSpells();
    for (const auto& s : spells)
        if (s.id == id) return &s;
    return nullptr;
}

const std::vector<MonsterDef>& Monsters() {
    static const std::vector<MonsterDef> monsters = BuildMonsters();
    return monsters;
}

const MonsterDef* FindMonster(std::string_view id) {
    for (const auto& m : Monsters())
        if (m.id == id) return &m;
    return nullptr;
}

std::vector<CharacterSheet> DefaultParty() {
    using DT = DamageType;
    std::vector<CharacterSheet> party;

    {   // Dwarf fighter: chain mail + shield, warhammer
        auto s = Base("Thorin", 3, {16, 12, 16, 10, 12, 8}, 18, 25, 31);
        SetSaves(s, {Ability::Str, Ability::Con});
        s.attacks = {Melee("warhammer", 5, "1d8+3", DT::Bludgeoning), Ranged("handaxe", 5, "1d6+3", DT::Slashing, 20, 60)};
        s.potions = 1;
        party.push_back(std::move(s)); }
    {   // Elf wizard
        auto s = Base("Elara", 3, {8, 14, 12, 16, 13, 10}, 12, 30, 17);
        SetSaves(s, {Ability::Int, Ability::Wis});
        s.spellAbility = Ability::Int;
        s.slotsMax[1] = s.slots[1] = 4; s.slotsMax[2] = s.slots[2] = 2;
        s.spells = {"fire_bolt", "magic_missile", "shatter"};
        s.attacks = {Melee("dagger", 4, "1d4+2", DT::Piercing, true)};
        party.push_back(std::move(s)); }
    {   // Human cleric
        auto s = Base("Aldric", 3, {14, 10, 14, 10, 16, 12}, 18, 30, 24);
        SetSaves(s, {Ability::Wis, Ability::Cha});
        s.spellAbility = Ability::Wis;
        s.slotsMax[1] = s.slots[1] = 4; s.slotsMax[2] = s.slots[2] = 2;
        s.spells = {"sacred_flame", "cure_wounds", "healing_word"};
        s.attacks = {Melee("mace", 4, "1d6+2", DT::Bludgeoning)};
        party.push_back(std::move(s)); }
    {   // Halfling rogue
        auto s = Base("Finn", 3, {8, 16, 14, 12, 12, 12}, 14, 25, 24);
        SetSaves(s, {Ability::Dex, Ability::Int});
        s.sneakAttackDice = 2;
        s.attacks = {Melee("shortsword", 5, "1d6+3", DT::Piercing, true), Ranged("shortbow", 5, "1d6+3", DT::Piercing, 80, 320)};
        s.potions = 1;
        party.push_back(std::move(s)); }
    return party;
}

}  // namespace dnd::Content
