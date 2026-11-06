#pragma once
#include <cstdint>
#include <optional>
#include <random>
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

// A self-contained, serializable random stream. Each Game owns one so concurrent games stay
// independent and a saved game resumes with the exact same future rolls.
class Rng {
public:
    explicit Rng(std::uint64_t seed = std::random_device{}()) { Seed(seed); }
    void Seed(std::uint64_t seed) { engine_.seed(seed); }
    int Uniform(int lo, int hi);  // portable (no std::uniform_int_distribution)
    std::string State() const;
    bool SetState(const std::string& s);

private:
    std::mt19937_64 engine_;
};

// Static facade. Rolls come from the Rng installed by the innermost live Scope on this thread,
// or from a mutex-guarded process-wide fallback when there is none (CLI, tests).
class DiceRoller {
public:
    class Scope {
    public:
        explicit Scope(Rng& rng);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        Rng* prev_;
    };

    static void Seed(std::uint64_t seed);  // seeds the fallback RNG
    static int Roll(int sides);                                    // 1..sides
    static int Roll(const DiceExpr& expr, bool critical = false);  // crit doubles the dice, not the bonus
    static D20Result D20(RollMode mode = RollMode::Normal);
    static int Percentile() { return Roll(100); }
    static int Index(int n);  // uniform in [0, n)
};

}  // namespace dnd
