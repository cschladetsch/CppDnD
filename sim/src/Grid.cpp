#include "dnd/Grid.hpp"

#include <algorithm>
#include <cstdlib>
#include <queue>

namespace dnd {

TacticalGrid::TacticalGrid(int width, int height)
    : w_(width),
      h_(height),
      cells_(static_cast<std::size_t>(width * height), Terrain::Floor),
      occupant_(static_cast<std::size_t>(width * height), -1) {}

bool TacticalGrid::IsBlocking(Pos p) const {
    if (!InBounds(p)) return true;
    return At(p) == Terrain::Wall || At(p) == Terrain::HalfCover || At(p) == Terrain::ThreeQuarterCover;
}

bool TacticalGrid::IsWalkableTerrain(Pos p) const {
    return InBounds(p) && (At(p) == Terrain::Floor || At(p) == Terrain::Difficult);
}

std::vector<Pos> TacticalGrid::Line(Pos a, Pos b) {
    std::vector<Pos> out;
    const int dx = std::abs(b.x - a.x), dy = -std::abs(b.y - a.y);
    const int sx = a.x < b.x ? 1 : -1, sy = a.y < b.y ? 1 : -1;
    int err = dx + dy;
    Pos p = a;
    for (;;) {
        out.push_back(p);
        if (p == b) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; p.x += sx; }
        if (e2 <= dx) { err += dx; p.y += sy; }
    }
    return out;
}

bool TacticalGrid::HasLineOfSight(Pos a, Pos b) const {
    auto clear = [this](const std::vector<Pos>& line) {
        for (std::size_t i = 1; i + 1 < line.size(); ++i)
            if (!InBounds(line[i]) || At(line[i]) == Terrain::Wall) return false;
        return true;
    };
    return clear(Line(a, b)) || clear(Line(b, a));
}

Cover TacticalGrid::CoverBetween(Pos attacker, Pos target) const {
    if (!HasLineOfSight(attacker, target)) return Cover::Total;
    const auto line = Line(attacker, target);
    Cover best = Cover::None;
    for (std::size_t i = 1; i + 1 < line.size(); ++i) {
        Cover c = Cover::None;
        switch (At(line[i])) {
            case Terrain::HalfCover: c = Cover::Half; break;
            case Terrain::ThreeQuarterCover: c = Cover::ThreeQuarters; break;
            default: break;
        }
        // Any other creature in the way grants half cover.
        if (OccupantAt(line[i]) >= 0 && c == Cover::None) c = Cover::Half;
        if (static_cast<int>(c) > static_cast<int>(best)) best = c;
    }
    return best;
}

int TacticalGrid::PathMap::CostTo(Pos p) const {
    if (width == 0 || p.x < 0 || p.y < 0 || p.x >= width) return kUnreachable;
    const auto i = static_cast<std::size_t>(p.y * width + p.x);
    return i < cost.size() ? cost[i] : kUnreachable;
}

std::vector<Pos> TacticalGrid::PathMap::PathTo(Pos p) const {
    std::vector<Pos> path;
    if (CostTo(p) == kUnreachable) return path;
    int i = p.y * width + p.x;
    const int start = origin.y * width + origin.x;
    while (i != start && i >= 0) {
        path.push_back(Pos{i % width, i / width});
        i = prev[static_cast<std::size_t>(i)];
    }
    std::reverse(path.begin(), path.end());
    return path;
}

TacticalGrid::PathMap TacticalGrid::ComputePaths(Pos from, const std::function<bool(Pos)>& passable) const {
    PathMap pm;
    pm.origin = from;
    pm.width = w_;
    pm.cost.assign(cells_.size(), kUnreachable);
    pm.prev.assign(cells_.size(), -1);
    if (!InBounds(from)) return pm;

    using Item = std::pair<int, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> pq;
    pm.cost[Idx(from)] = 0;
    pq.push({0, static_cast<int>(Idx(from))});

    while (!pq.empty()) {
        const auto [c, i] = pq.top();
        pq.pop();
        if (c != pm.cost[static_cast<std::size_t>(i)]) continue;
        const Pos cur{i % w_, i / w_};
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) continue;
                const Pos n{cur.x + dx, cur.y + dy};
                if (!IsWalkableTerrain(n) || !passable(n)) continue;
                // No squeezing diagonally between two solid corners.
                if (dx && dy && IsBlocking({cur.x + dx, cur.y}) && IsBlocking({cur.x, cur.y + dy})) continue;
                const int nc = c + EnterCostFt(n);
                const auto ni = Idx(n);
                if (nc < pm.cost[ni]) {
                    pm.cost[ni] = nc;
                    pm.prev[ni] = i;
                    pq.push({nc, static_cast<int>(ni)});
                }
            }
        }
    }
    return pm;
}

}  // namespace dnd
