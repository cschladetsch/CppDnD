#include "dnd/Dice.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <random>

namespace dnd {
namespace {
std::mutex g_mutex;
std::mt19937_64 g_engine{std::random_device{}()};

int Uniform(int lo, int hi) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return std::uniform_int_distribution<int>(lo, hi)(g_engine);
}
}  // namespace

void DiceRoller::Seed(std::uint64_t seed) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_engine.seed(seed);
}

int DiceRoller::Roll(int sides) { return sides <= 0 ? 0 : Uniform(1, sides); }

int DiceRoller::Index(int n) { return n <= 1 ? 0 : Uniform(0, n - 1); }

int DiceRoller::Roll(const DiceExpr& e, bool critical) {
    const int dice = critical ? e.count * 2 : e.count;
    int total = e.bonus;
    for (int i = 0; i < dice; ++i) total += Roll(e.sides);
    return std::max(0, total);
}

D20Result DiceRoller::D20(RollMode mode) {
    D20Result r;
    r.mode = mode;
    const int a = Roll(20);
    if (mode == RollMode::Normal) {
        r.natural = r.discarded = a;
        return r;
    }
    const int b = Roll(20);
    r.natural = mode == RollMode::Advantage ? std::max(a, b) : std::min(a, b);
    r.discarded = a + b - r.natural;
    return r;
}

std::optional<DiceExpr> DiceExpr::Parse(std::string_view text) {
    std::string t;
    for (char c : text)
        if (!std::isspace(static_cast<unsigned char>(c))) t += c;
    if (t.empty()) return std::nullopt;

    std::size_t i = 0;
    auto readInt = [&](int& out) {
        const std::size_t start = i;
        while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) ++i;
        if (start == i || i - start > 6) return false;
        out = std::stoi(t.substr(start, i - start));
        return true;
    };

    DiceExpr e;
    int first = 0;
    if (!readInt(first)) return std::nullopt;
    if (i < t.size() && (t[i] == 'd' || t[i] == 'D')) {
        ++i;
        e.count = first;
        if (!readInt(e.sides)) return std::nullopt;
    } else {
        if (i != t.size()) return std::nullopt;
        e.bonus = first;
        return e;
    }
    if (i < t.size()) {
        const char sign = t[i++];
        int b = 0;
        if ((sign != '+' && sign != '-') || !readInt(b) || i != t.size()) return std::nullopt;
        e.bonus = sign == '-' ? -b : b;
    }
    return e;
}

std::string DiceExpr::ToString() const {
    std::string s = std::to_string(count) + "d" + std::to_string(sides);
    if (bonus > 0) s += "+" + std::to_string(bonus);
    if (bonus < 0) s += std::to_string(bonus);
    return s;
}

}  // namespace dnd
