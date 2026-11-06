#pragma once
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <vector>

#include "dnd/Types.hpp"

namespace dnd {

enum class Terrain : std::uint8_t { Floor, Wall, Difficult, HalfCover, ThreeQuarterCover };
enum class Cover { None, Half, ThreeQuarters, Total };

constexpr int kUnreachable = std::numeric_limits<int>::max();

// 2D grid of 5 ft squares. Cover objects block movement but not sight.
class TacticalGrid {
public:
    struct PathMap {
        Pos origin;
        int width = 0;
        std::vector<int> cost;  // feet from origin, kUnreachable if none
        std::vector<int> prev;
        int CostTo(Pos p) const;
        std::vector<Pos> PathTo(Pos p) const;  // excludes origin, includes p; empty if unreachable
    };

    TacticalGrid(int width, int height);

    int Width() const { return w_; }
    int Height() const { return h_; }
    bool InBounds(Pos p) const { return p.x >= 0 && p.y >= 0 && p.x < w_ && p.y < h_; }

    Terrain At(Pos p) const { return cells_[Idx(p)]; }
    void Set(Pos p, Terrain t) { cells_[Idx(p)] = t; }

    int OccupantAt(Pos p) const { return InBounds(p) ? occupant_[Idx(p)] : -1; }
    void SetOccupant(Pos p, int id) { occupant_[Idx(p)] = id; }

    bool IsBlocking(Pos p) const;         // out of bounds, wall or cover object
    bool IsWalkableTerrain(Pos p) const;  // floor or difficult
    int EnterCostFt(Pos p) const { return At(p) == Terrain::Difficult ? 10 : 5; }

    static std::vector<Pos> Line(Pos a, Pos b);  // Bresenham, inclusive of both ends
    bool HasLineOfSight(Pos a, Pos b) const;     // walls block; either direction may be clear
    Cover CoverBetween(Pos attacker, Pos target) const;

    PathMap ComputePaths(Pos from, const std::function<bool(Pos)>& passable) const;

private:
    std::size_t Idx(Pos p) const { return static_cast<std::size_t>(p.y * w_ + p.x); }

    int w_, h_;
    std::vector<Terrain> cells_;
    std::vector<int> occupant_;
};

}  // namespace dnd
