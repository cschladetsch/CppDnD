# src

Implementation of the headers in [../include/dnd](../include/README.md), plus the two programs.

| File | Implements |
|---|---|
| `Types.cpp`, `Dice.cpp`, `Grid.cpp`, `Actor.cpp` | basic types, dice and RNG, the grid, actors |
| `Combat.cpp` | initiative, action resolution, conditions, concentration, death saves, opportunity attacks |
| `AI.cpp` | the tactical AI (target scoring, movement, ranged and support logic) |
| `DM.cpp` | encounter building from an XP budget |
| `Human.cpp` | the terminal prompt controller |
| `Content.cpp` | loading and validating `data/` (errors name the file and field), class progression tables |
| `Game.cpp` | exploration, clock, travel, rests, events, flags, quests, autopilot, save and load |
| `GameView.cpp` | the JSON view of a `Game` returned to clients |
| `Json.cpp` | JSON conversion for sheets, events, actions and combat |
| `Images.cpp` | the image clients, prompts built from the content, the drawing queue, model resolution through the model store |
| `History.cpp` | the pair and group histories, digests for agent prompts, atomic file writes |
| `Agents.cpp` | agents.json loading and inheritance, the agents, the message hub, per-actor combat control, the party council, NPC voices, runtime model switching |
| `Llm.cpp` | HTTP LLM client, narrator, LLM Dungeon Master, plain-English `Say` |
| `main.cpp` | `dnd_sim`: the terminal client (`--help` lists the options) |
| `server_main.cpp` | `dnd_server`: the HTTP routes and static file serving |

## Conventions

- Rules code rolls only through `DiceRoller`, which uses the current game's seeded `Rng`. A process-wide fallback exists for the CLI and tests.
- Model output is untrusted: anything parsed from it goes through the same validation as a client request.
- Platform notes: the Windows build defines `_WIN32_WINNT`, `NOMINMAX` and `_CRT_SECURE_NO_WARNINGS` and links `ws2_32` (see `CMakeLists.txt`). Parse RNG state through a temporary, because MSVC's `operator>>` on `mt19937_64` throws on bad input.
