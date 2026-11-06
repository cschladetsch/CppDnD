# CppDnD

A single-player Dungeons & Dragons 5e tactical simulator in C++20. You can play as a party member or as the Dungeon Master, and swap between the two (or hand everything to the AI) in the middle of a fight.

Design notes: [dungeons_dragons_c_simulator_architecture.md](dungeons_dragons_c_simulator_architecture.md)

## Features

- Controller pattern: every actor and the DM engine is driven by an `IController`, either `HumanController` (text prompts) or `AIController` (utility scoring). Controllers are hot-swappable at runtime.
- Rules engine: d20 rolls with advantage and disadvantage, crits, cover, opportunity attacks, sneak attack, death saves, spell slots, and the action/bonus/reaction/movement economy.
- Tactical grid: 5 ft squares, Bresenham line of sight, half and three-quarters cover, difficult terrain, Dijkstra pathfinding.
- DM engine: DMG 2014 XP-budget encounter generator (Easy to Deadly) and event-driven narration.
- Deterministic: one seeded RNG, so `--seed N` reproduces a fight exactly.
- Sample content: level 3 fighter, wizard, cleric and rogue; 7 SRD monsters; 6 spells.

## Build

The engine lives in `sim/` and has its own CMake project. It needs CMake 3.20+ and a C++20 compiler, with no external dependencies.

```powershell
cmake -S sim -B sim/build -G Ninja -DCMAKE_CXX_COMPILER=clang++
cmake --build sim/build
ctest --test-dir sim/build
```

> The `CMakeLists.txt` at the repo root belongs to a separate LLM-driven design (`CppAiDungeon`) and is not used by the simulator.

## Run

```powershell
.\sim\build\dnd_sim.exe --role player --hero Elara --difficulty hard --seed 7
```

| Option | Meaning |
|---|---|
| `--role player\|dm\|auto` | `player`: you drive one hero. `dm`: you drive every monster. `auto`: watch the AI play. |
| `--hero <name>` | Hero to control in player mode (Thorin, Elara, Aldric, Finn). |
| `--difficulty easy\|medium\|hard\|deadly` | Encounter budget. |
| `--seed <n>` | Reproducible run. |
| `--quiet` | No narration, one-line result. |

### Prompt commands

```
m <x> <y>                    move toward a square
a <id> [attack#]             attack target id
c <spell#|name> <id> [slot]  cast a spell at target id
p [id]                       drink a healing potion
dash | dodge | e             dash, dodge, end turn
map | help
swap player|dm|auto          change your role mid-fight
```

## Layout

```
sim/
  include/dnd/   headers (Dice, Types, Grid, Actor, Combat, AI, Human, DM, Session, Content)
  src/           implementation and main.cpp
  tests/         dependency-free test runner
```

## Limitations (first pass)

- One attack per action (no Extra Attack); no conditions beyond downed and dodging; no concentration.
- Cover objects block movement and grant cover but not sight; only walls block sight.
- Encounter builder ignores the DMG party-size multiplier adjustment.
- Monster tuning is lenient: the sample party wins nearly every fight.

## Roadmap

- Data-driven content (JSON monsters, spells, characters)
- More classes, Extra Attack, conditions, concentration
- Save and load, multi-encounter dungeons
- LLM-backed narration and DM controller
