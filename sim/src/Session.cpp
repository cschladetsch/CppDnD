#include "dnd/Session.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <map>

#include "dnd/AI.hpp"
#include "dnd/Content.hpp"
#include "dnd/Human.hpp"
#include "dnd/Util.hpp"

namespace dnd {

Session::Session(std::istream& in, std::ostream& out, std::uint64_t seed)
    : in_(in), out_(out), dm_(std::make_unique<AIController>(), seed) {
    DiceRoller::Seed(seed);
    bus_.Subscribe([this](const Event& e) { OnEvent(e); });
}

void Session::SetRole(Role r) {
    role_ = r;
    dm_.SetController(r == Role::DungeonMaster ? std::unique_ptr<IController>(std::make_unique<HumanController>(in_, out_))
                                               : std::unique_ptr<IController>(std::make_unique<AIController>()));
    if (combat_) AssignControllers();
}

void Session::OnRoleSwap(Role r) {
    SetRole(r);
    out_ << "\n[role is now: " << ToString(r) << "]\n";
}

void Session::OnEvent(const Event& e) {
    if (quiet_) return;
    out_ << dm_.Narrate(e) << '\n';
    if (e.type == EventType::RoundStart && role_ == Role::Spectator && combat_) out_ << combat_->RenderMap();
}

TacticalGrid Session::BuildRoom() {
    TacticalGrid g(20, 12);
    for (Pos p : {Pos{9, 3}, Pos{10, 3}, Pos{9, 8}, Pos{10, 8}}) g.Set(p, Terrain::HalfCover);   // crates
    for (Pos p : {Pos{8, 6}, Pos{12, 5}}) g.Set(p, Terrain::ThreeQuarterCover);                  // pillars
    for (Pos p : {Pos{14, 6}, Pos{14, 7}}) g.Set(p, Terrain::Wall);                              // wall stub
    for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx) {
            g.Set({13 + dx, 9 + dy}, Terrain::Difficult);                                        // rubble
            g.Set({5 + dx, 2 + dy}, Terrain::Difficult);
        }
    return g;
}

void Session::AssignControllers() {
    if (!combat_) return;
    GameActor* hero = nullptr;
    auto lower = [](std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); }); return s; };
    for (const auto& a : combat_->Actors())
        if (a->team == Team::Party && lower(a->Name()) == lower(hero_) && !a->IsDead()) hero = a.get();
    if (!hero)
        for (const auto& a : combat_->Actors())
            if (a->team == Team::Party && !a->IsDead()) { hero = a.get(); break; }

    for (const auto& a : combat_->Actors()) {
        if (a->team == Team::Party) {
            const bool human = role_ == Role::Player && a.get() == hero;
            a->controller = human ? std::unique_ptr<IController>(std::make_unique<HumanController>(in_, out_))
                                  : std::unique_ptr<IController>(std::make_unique<AIController>());
        } else {
            a->controller = dm_.MakeMonsterController();
        }
    }
}

CombatResult Session::RunEncounter(Difficulty d) {
    auto party = Content::DefaultParty();
    std::vector<int> levels;
    for (const auto& p : party) levels.push_back(p.level);

    const Encounter enc = dm_.Generate(levels, d);
    if (!quiet_) out_ << dm_.Describe(enc, levels) << '\n';

    combat_ = std::make_unique<Combat>(BuildRoom(), bus_);
    combat_->SetRoleSwapHandler([this](Role r) { OnRoleSwap(r); });

    int row = 0;
    for (auto& sheet : party) {
        const int ys[] = {3, 5, 7, 9};
        combat_->AddActor(Team::Party, std::move(sheet), Pos{row % 2 == 0 ? 1 : 2, ys[row % 4]}, nullptr);
        ++row;
    }

    std::map<std::string, int> total;
    for (const auto* m : enc.monsters) ++total[m->name];
    std::map<std::string, int> seen;
    for (const auto* m : enc.monsters) {
        CharacterSheet sheet = m->sheet;
        sheet.name = total[m->name] > 1 ? Cat(m->name, " ", ++seen[m->name]) : m->name;
        Pos p;
        const auto& grid = combat_->Grid();
        do {
            p = Pos{15 + DiceRoller::Index(4), 1 + DiceRoller::Index(10)};
        } while (!grid.IsWalkableTerrain(p) || grid.OccupantAt(p) >= 0);
        combat_->AddActor(Team::Monsters, std::move(sheet), p, nullptr);
    }

    AssignControllers();
    const CombatResult result = combat_->Run();
    combat_.reset();
    return result;
}

}  // namespace dnd
