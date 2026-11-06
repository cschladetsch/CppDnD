#pragma once
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "dnd/Content.hpp"
#include "dnd/Controller.hpp"
#include "dnd/Events.hpp"

namespace dnd {

enum class Difficulty { Easy, Medium, Hard, Deadly };
std::string_view ToString(Difficulty d);
std::optional<Difficulty> ParseDifficulty(std::string_view s);

struct Encounter {
    std::vector<const MonsterDef*> monsters;
    int rawXp = 0;
    int adjustedXp = 0;
    Difficulty target = Difficulty::Medium;
};

// Builds encounters from an XP budget and narrates combat events. It owns an IController whose
// kind (AI or Human) is cloned onto every monster, so swapping it swaps who plays the monsters.
class DungeonMasterEngine {
public:
    explicit DungeonMasterEngine(std::unique_ptr<IController> controller, std::uint64_t flavourSeed = 1);

    // DMG (2014) encounter-building method.
    static int XpThreshold(int level, Difficulty d);
    static int PartyThreshold(const std::vector<int>& levels, Difficulty d);
    static double Multiplier(std::size_t monsterCount);
    static int AdjustedXp(const std::vector<const MonsterDef*>& monsters);

    Encounter Generate(const std::vector<int>& partyLevels, Difficulty d) const;
    std::string Describe(const Encounter& e, const std::vector<int>& partyLevels) const;
    std::string Narrate(const Event& e);

    void SetController(std::unique_ptr<IController> c) { controller_ = std::move(c); }
    IController& Controller() { return *controller_; }
    std::unique_ptr<IController> MakeMonsterController() const { return controller_->Clone(); }

private:
    std::unique_ptr<IController> controller_;
    std::mt19937 flavour_;  // separate from the dice RNG so prose never perturbs the simulation
};

}  // namespace dnd
