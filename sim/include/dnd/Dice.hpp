#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dnd {

enum class RollMode { Normal, Advantage, Disadvantage };

// Advantage and disadvantage cancel regardless of how many sources there are.
constexpr RollMode CombineModes(bool advantage, bool disadvantage) {
    if (advantage == disadvantage) return RollMode::Normal;
    return advantage ? RollMode::Advantage : RollMode::Disadvantage;
}

// NdS+M
struct DiceExpr {
    int count = 0;
    int sides = 0;
    int bonus = 0;

    static std::optional<DiceExpr> Parse(std::string_view text);  // "2d6+3", "1d8", "5"
    std::string ToString() const;
    double Average() const { return count * (sides + 1) / 2.0 + bonus; }
    DiceExpr WithExtraDice(int extra) const { return DiceExpr{count + extra, sides, bonus}; }
};

struct D20Result {
    int natural = 0;    // the die that was kept
    int discarded = 0;  // the other die (== natural for a Normal roll)
    RollMode mode = RollMode::Normal;
    bool IsCrit() const { return natural == 20; }
    bool IsFumble() const { return natural == 1; }
};

// One process-wide, mutex-guarded RNG. Seed() makes a whole run reproducible.
class DiceRoller {
public:
    static void Seed(std::uint64_t seed);
    static int Roll(int sides);                                    // 1..sides
    static int Roll(const DiceExpr& expr, bool critical = false);  // crit doubles the dice, not the bonus
    static D20Result D20(RollMode mode = RollMode::Normal);
    static int Percentile() { return Roll(100); }
    static int Index(int n);  // uniform in [0, n)
};

}  // namespace dnd
