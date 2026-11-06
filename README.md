# CppDnD

A Dungeons & Dragons 5e simulator in C++20, built as a backend for a web game. One engine, two front ends: a terminal client (`dnd_sim`) and an HTTP/JSON server (`dnd_server`). You can play a party member or the Dungeon Master, swap roles mid-fight, or hand everything to the AI.

Design notes: [dungeons_dragons_c_simulator_architecture.md](dungeons_dragons_c_simulator_architecture.md)

## Features

- **Data-driven content.** Monsters, spells, classes, weapons, characters and modules are JSON under `sim/data/`. Load-time validation reports the file and field of any mistake.
- **Classes.** Fighter, barbarian, rogue, ranger, wizard, cleric, built from class tables at any level 1 to 20: hit points, proficiency, spell slots, Extra Attack, sneak attack, Rage, Second Wind.
- **Rules.** Advantage and disadvantage, crits, cover, opportunity attacks, death saves, conditions (blinded, frightened, incapacitated, paralyzed, poisoned, prone, restrained, stunned), concentration with damage saves, resistance and vulnerability, exhaustion, short and long rests with Hit Dice, travel pace and forced marches.
- **Modules.** A module is the space for a long-running session: a graph of locations on a clock, with scripted and random encounters, NPC dialogue, flags, quests, seeded secrets and an end condition. `ravenmoor` is an original gothic-horror module in the spirit of Ravenloft.
- **Step-driven combat.** `Advance` runs until a remote human must act; `Submit` answers. Nothing blocks, so a web server can drive many games.
- **Save and load.** The complete state, including the RNG stream, round-trips through JSON. A reloaded game plays out identically.
- **LLM narration and LLM Dungeon Master.** Any OpenAI-compatible server (llama.cpp, Ollama, vLLM). The model proposes; the rules engine disposes. Illegal or malformed plans fall back to the built-in AI, and a dead model never stalls a fight. Models are located through [CppLmmModelStore](https://github.com/cschladetsch/CppLmmModelStore).
- **Deterministic.** Each game owns a seeded, portable RNG, so `--seed N` reproduces a run exactly.

## Build

Needs CMake 3.20+, a C++20 compiler and git (the model store is fetched on configure).

```powershell
cmake -S sim -B sim/build -G Ninja -DCMAKE_CXX_COMPILER=clang++
cmake --build sim/build
ctest --test-dir sim/build
```

Offline or already have the model store checked out: add `-DDND_MODELSTORE_DIR=<path>`. To build without it: `-DDND_USE_MODELSTORE=OFF`.

> The `CMakeLists.txt` at the repo root belongs to a separate LLM-driven design (`CppAiDungeon`) and is not used by the simulator.

## Terminal client

```powershell
.\sim\build\dnd_sim.exe --list
.\sim\build\dnd_sim.exe --module ravenmoor --role player --hero elara --seed 7
.\sim\build\dnd_sim.exe --module ravenmoor --role auto --quiet      # watch the AI play the whole module
.\sim\build\dnd_sim.exe --rooms 3 --difficulty hard                 # quick procedural delve
```

Exploration commands: `look`, `go <place>`, `search`, `talk <npc> [topic]`, `rest short|long`, `wait <hours>`, `pace slow|normal|fast`, `party`, `quests`, `auto [n]`, `save <file>`, `quit`. In a fight, `help` lists the turn commands (`m`, `a`, `c`, `f rage|second_wind`, `p`, `dash`, `dodge`, `e`, `swap player|dm|auto`). Resume with `--load <file>`.

## Web backend

```powershell
.\sim\build\dnd_server.exe --port 8088 --static .\web      # --static is optional
curl -X POST localhost:8088/api/games -d '{"module":"ravenmoor","role":"player","hero":"elara","seed":7}'
```

The full endpoint reference is in [sim/docs/API.md](sim/docs/API.md). `python sim/tests/api_smoke.py http://127.0.0.1:8088` plays a whole module through the API.

### LLM

```powershell
$env:DND_LLM_URL   = "http://127.0.0.1:11434"        # Ollama; llama-server defaults to :8080
$env:DND_LLM_MODEL = "deepseek-r1"
.\sim\build\dnd_server.exe
# then create games with "llmNarration": true and/or "llmDm": true
```

Reasoning models are supported: `reasoning_content` deltas are parsed by the model store's stream parser and never shown. `GET /api/llm` reports the model's location in the store (`DEEPSEEK_MODEL_HOME`). `--mock-llm` runs the full pipeline without a model.

## Layout

```
sim/
  data/          classes, weapons, spells, monsters, characters, modules/*.json
  include/dnd/   Dice, Types, Grid, Actor, Combat, AI, Human, DM, Content, Json, Game, Llm
  src/           implementation, main.cpp (terminal), server_main.cpp (HTTP)
  tests/         tests.cpp (unit and integration), api_smoke.py (HTTP end to end)
  third_party/   nlohmann/json, cpp-httplib (vendored, MIT)
  docs/API.md    HTTP reference
```

## Authoring content

Add a file under `sim/data/modules/`, restart, and it appears in `GET /api/content`. See `ravenmoor.json` for every feature: locations with tactical maps (`.` floor, `#` wall, `~` rough, `c` half cover, `O` three-quarters cover, `P` party start, `M` monster spawn), exits with miles or hours and flag requirements, `enter` and `search` events, NPC topics, random tables by day and night, quests and `endWhen`.

## Limitations

- Vampire Spawn's bite is simplified (no necrotic damage or maximum-HP drain).
- Spells are the ten in `spells.json`; classes have no subclasses or feats.
- Cover objects block movement and grant cover but not sight; only walls block sight.
- Dead characters stay dead (no resurrection); a total party kill ends the game.
- The LLM client speaks plain HTTP; put a proxy in front for TLS.
