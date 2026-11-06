# CppDnd simulator (first pass)

Single-player D&D 5e tactical engine following `../dungeons_dragons_c_simulator_architecture.md`.
Self-contained: it does not use the repo-root `CMakeLists.txt`.

    cmake -S sim -B sim/build -G Ninja -DCMAKE_CXX_COMPILER=clang++
    cmake --build sim/build
    sim/build/dnd_sim --role auto --difficulty hard --seed 7
    sim/build/dnd_sim --role player --hero Elara
    sim/build/dnd_sim --role dm            # you drive every monster
    ctest --test-dir sim/build

## Layout

| Architecture doc            | Code                                              |
|-----------------------------|---------------------------------------------------|
| `DiceRoller`                | `Dice.hpp/.cpp` (seedable, adv/dis, `2d6+3` parser) |
| `CharacterSheet`            | `Types.hpp` (stats, action economy, slots, attacks)  |
| `GameActor`                 | `Actor.hpp/.cpp` (HP, death saves, healing)          |
| `TacticalGrid`              | `Grid.hpp/.cpp` (Bresenham LOS, cover, Dijkstra)     |
| Initiative / rules engine   | `Combat.hpp/.cpp` (turn loop, opportunity attacks)   |
| Event dispatcher            | `Events.hpp` (`EventBus`)                            |
| `IController`               | `Controller.hpp`; `AI.cpp`, `Human.cpp`              |
| `DungeonMasterEngine`       | `DM.hpp/.cpp` (XP budgets, narration)                |
| Role swapping               | `Session.hpp/.cpp` (`swap player|dm|auto` at a prompt) |
| Sample content              | `Content.cpp` (4 PCs, 7 SRD monsters, 6 spells)      |

## Known simplifications
- DMG 2014 XP-budget method (no party-size multiplier adjustment); levels 1-20 thresholds included.
- One attack per action (no Extra Attack), no conditions beyond downed/dodging, no concentration.
- Cover objects block movement and give cover but do not block sight; only walls block sight.
- Burst radius uses a Euclidean cell test, not true 5e sphere geometry.
