#include "dnd/DM.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>

#include "dnd/Dice.hpp"
#include "dnd/Util.hpp"

namespace dnd {
namespace {

// Easy, Medium, Hard, Deadly per character level 1..20.
constexpr std::array<std::array<int, 4>, 20> kThresholds{{
    {25, 50, 75, 100},       {50, 100, 150, 200},     {75, 150, 225, 400},     {125, 250, 375, 500},
    {250, 500, 750, 1100},   {300, 600, 900, 1400},   {350, 750, 1100, 1700},  {450, 900, 1400, 2100},
    {550, 1100, 1600, 2400}, {600, 1200, 1900, 2800}, {800, 1600, 2400, 3600}, {1000, 2000, 3000, 4500},
    {1100, 2200, 3400, 5100},{1250, 2500, 3800, 5700},{1400, 2800, 4300, 6400},{1600, 3200, 4800, 7200},
    {2000, 3900, 5900, 8800},{2100, 4200, 6300, 9500},{2400, 4900, 7300, 10900},{2800, 5700, 8500, 12700},
}};

constexpr std::array<const char*, 4> kDeathFlavour{
    "The blow lands with a sickening finality.", "Silence follows, broken only by ragged breathing.",
    "The echo of the strike rolls down the corridor.", "It collapses and does not move again."};
constexpr std::array<const char*, 4> kDownFlavour{
    "Vision swims; the world narrows to a pinprick.", "A wet gasp, and the fight goes on without them.",
    "They crumple, fingers scrabbling at the flagstones.", "Darkness takes them, but not quite yet."};

}  // namespace

std::string_view ToString(Difficulty d) {
    switch (d) {
        case Difficulty::Easy: return "Easy";
        case Difficulty::Medium: return "Medium";
        case Difficulty::Hard: return "Hard";
        case Difficulty::Deadly: return "Deadly";
    }
    return "?";
}

std::optional<Difficulty> ParseDifficulty(std::string_view s) {
    std::string t(s);
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (t == "easy") return Difficulty::Easy;
    if (t == "medium") return Difficulty::Medium;
    if (t == "hard") return Difficulty::Hard;
    if (t == "deadly") return Difficulty::Deadly;
    return std::nullopt;
}

DungeonMasterEngine::DungeonMasterEngine(std::unique_ptr<IController> controller, std::uint64_t flavourSeed)
    : controller_(std::move(controller)), flavour_(static_cast<std::mt19937::result_type>(flavourSeed)) {}

int DungeonMasterEngine::XpThreshold(int level, Difficulty d) {
    level = std::clamp(level, 1, 20);
    return kThresholds[static_cast<std::size_t>(level - 1)][static_cast<std::size_t>(d)];
}

int DungeonMasterEngine::PartyThreshold(const std::vector<int>& levels, Difficulty d) {
    int sum = 0;
    for (int l : levels) sum += XpThreshold(l, d);
    return sum;
}

double DungeonMasterEngine::Multiplier(std::size_t n) {
    if (n <= 1) return 1.0;
    if (n == 2) return 1.5;
    if (n <= 6) return 2.0;
    if (n <= 10) return 2.5;
    if (n <= 14) return 3.0;
    return 4.0;
}

int DungeonMasterEngine::AdjustedXp(const std::vector<const MonsterDef*>& monsters) {
    int raw = 0;
    for (const auto* m : monsters) raw += m->xp;
    return static_cast<int>(raw * Multiplier(monsters.size()));
}

Encounter DungeonMasterEngine::Generate(const std::vector<int>& levels, Difficulty d) const {
    const auto& pool = Content::Monsters();
    const int lo = PartyThreshold(levels, d);
    const int hi = d == Difficulty::Deadly ? lo * 3 / 2 : PartyThreshold(levels, static_cast<Difficulty>(static_cast<int>(d) + 1));
    const int mid = (lo + hi) / 2;

    Encounter best;
    best.target = d;
    int bestGap = std::numeric_limits<int>::max();

    for (int attempt = 0; attempt < 400; ++attempt) {
        // One or two monster types per encounter keeps fights readable.
        const int types = 1 + DiceRoller::Index(2);
        std::vector<const MonsterDef*> kinds;
        for (int i = 0; i < types; ++i) kinds.push_back(&pool[static_cast<std::size_t>(DiceRoller::Index(static_cast<int>(pool.size())))]);

        std::vector<const MonsterDef*> picks;
        while (picks.size() < 10) {
            picks.push_back(kinds[static_cast<std::size_t>(DiceRoller::Index(static_cast<int>(kinds.size())))]);
            const int adj = AdjustedXp(picks);
            const int gap = std::abs(adj - mid);
            if (gap < bestGap && adj >= lo && adj < hi) {
                bestGap = gap;
                best.monsters = picks;
                best.adjustedXp = adj;
            }
            if (adj >= hi) break;
        }
        if (bestGap < (hi - lo) / 4) break;  // close enough
    }
    if (best.monsters.empty()) best.monsters.push_back(&pool.front());  // degenerate fallback
    best.rawXp = 0;
    for (const auto* m : best.monsters) best.rawXp += m->xp;
    best.adjustedXp = AdjustedXp(best.monsters);
    return best;
}

std::string DungeonMasterEngine::Describe(const Encounter& e, const std::vector<int>& levels) const {
    std::map<std::string, int> counts;
    for (const auto* m : e.monsters) ++counts[m->name];
    std::string list;
    for (const auto& [name, n] : counts) list += Cat(list.empty() ? "" : ", ", n, " x ", name);
    return Cat("Encounter (", ToString(e.target), "): ", list, "\n  raw XP ", e.rawXp, ", adjusted ", e.adjustedXp,
               "  [party thresholds  Easy ", PartyThreshold(levels, Difficulty::Easy), "  Medium ", PartyThreshold(levels, Difficulty::Medium),
               "  Hard ", PartyThreshold(levels, Difficulty::Hard), "  Deadly ", PartyThreshold(levels, Difficulty::Deadly), "]");
}

std::string DungeonMasterEngine::Narrate(const Event& e) {
    auto pick = [&](const auto& arr) { return arr[flavour_() % arr.size()]; };
    switch (e.type) {
        case EventType::CombatStart: return "\n*** " + e.text + " ***";
        case EventType::RoundStart: return Cat("\n=== Round ", e.amount, " ===");
        case EventType::TurnStart: return "-- " + e.text;
        case EventType::Down: return "   " + e.text + "\n   " + pick(kDownFlavour);
        case EventType::Death: return "   " + e.text + "\n   " + pick(kDeathFlavour);
        case EventType::CombatEnd: return "\n*** " + e.text + " ***";
        default: return "   " + e.text;
    }
}

}  // namespace dnd
