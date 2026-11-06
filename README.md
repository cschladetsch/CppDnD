# CppDnD

A Dungeons & Dragons 5e simulator in C++20, built as a backend for a web game. One engine, three ways in: a terminal client (`dnd_sim`), an HTTP/JSON server (`dnd_server`) that also serves a browser client, and a plain-English prompt backed by a local model. You can play a party member or the Dungeon Master, swap roles mid-fight, or hand everything to the AI.

Design notes: [dungeons_dragons_c_simulator_architecture.md](dungeons_dragons_c_simulator_architecture.md)

## Quick start

```powershell
.\run.ps1                              # build if needed, then watch the AI play a 3 room delve
.\run.ps1 --interactive                # play it yourself (one hero, you choose each action)
.\run.ps1 --module ravenmoor --role player --hero elara --seed 7
.\run.ps1 -Server --port 8088          # web client at http://127.0.0.1:8088/
.\test.ps1                           # build, then run every test
```

`run.ps1` configures with Ninja and clang++ on first use. `-Rebuild` wipes `sim/build` first. Everything after the switches goes to the program.

## Features

- **Data-driven content.** Monsters, spells, classes, weapons, characters and modules are JSON under `sim/data/`. Load-time validation reports the file and field of any mistake. See [sim/data/README.md](sim/data/README.md).
- **Classes.** Fighter, barbarian, rogue, ranger, wizard, cleric, built from class tables at any level 1 to 20: hit points, proficiency, spell slots, Extra Attack, sneak attack, Rage, Second Wind.
- **Rules.** Advantage and disadvantage, crits, cover, opportunity attacks, death saves, conditions (blinded, frightened, incapacitated, paralyzed, poisoned, prone, restrained, stunned), concentration with damage saves, resistance and vulnerability, exhaustion, short and long rests with Hit Dice, travel pace and forced marches.
- **Modules.** A module is the space for a long-running session: a graph of locations on a clock, with scripted and random encounters, NPC dialogue, flags, quests, seeded secrets and an end condition. `ravenmoor` is an original gothic-horror module in the spirit of Ravenloft.
- **Step-driven combat.** `Advance` runs until a remote human must act; `Submit` answers. Nothing blocks, so a web server can drive many games.
- **Save and load.** The complete state, including the RNG stream, round-trips through JSON. A reloaded game plays out identically.
- **Web client.** A single-page client in [sim/web/](sim/web/README.md), served by the server at `/`.
- **Plain English.** Type "go to the village and ask Marta about the rumours" or "hit the ghoul with my axe"; a model turns it into game commands and the rules engine validates each one.
- **LLM narration and LLM Dungeon Master.** Any OpenAI-compatible server (llama.cpp, Ollama, vLLM). The model proposes; the rules engine disposes. Illegal or malformed plans fall back to the built-in AI, and a dead model never stalls a fight. Models are located through [CppLmmModelStore](https://github.com/cschladetsch/CppLmmModelStore).
- **Deterministic.** Each game owns a seeded, portable RNG, so `--seed N` reproduces a run exactly.

## Build

Needs CMake 3.20+, a C++20 compiler and git (the model store is fetched on configure). The scripts above do this for you; by hand:

```powershell
cmake -S sim -B sim/build -G Ninja -DCMAKE_CXX_COMPILER=clang++
cmake --build sim/build
ctest --test-dir sim/build --output-on-failure
```

Offline or already have the model store checked out: add `-DDND_MODELSTORE_DIR=<path>`. To build without it: `-DDND_USE_MODELSTORE=OFF`.

> The `CMakeLists.txt` at the repo root belongs to a separate LLM-driven design (`CppAiDungeon`) and is not used by the simulator.

## Terminal client

```powershell
.\run.ps1 --list                                                  # modules, characters, monsters
.\run.ps1 --module ravenmoor --role player --hero elara --seed 7
.\run.ps1 --interactive --module ravenmoor                     # same as --role player
.\run.ps1 --module ravenmoor --role auto --quiet                  # watch the AI play the whole module
.\run.ps1 --rooms 3 --difficulty hard                             # quick procedural delve
.\run.ps1 --module ravenmoor --role player --hero elara --llm-say # plain English at the prompt
```

Exploration commands: `look`, `go <place>`, `search`, `talk <npc> [topic]`, `rest short|long`, `wait <hours>`, `pace slow|normal|fast`, `party`, `quests`, `auto [n]`, `save <file>`, `quit`. In a fight, `help` lists the turn commands (`m`, `a`, `c`, `f rage|second_wind`, `p`, `dash`, `dodge`, `e`, `swap player|dm|auto`). Resume with `--load <file>`.

## Web server and client

```powershell
.\run.ps1 -Server --port 8088
```

Open http://127.0.0.1:8088/ for the bundled client, or talk to the JSON API directly:

```powershell
curl -X POST localhost:8088/api/games -d '{"module":"ravenmoor","role":"player","hero":"elara","seed":7}'
```

Server flags: `--port`, `--host`, `--data <dir>`, `--static <dir>` (serve a different front end; also `DND_WEB`), `--llm-url`, `--llm-model`, `--ollama <model>`, `--mock-llm`. The endpoint reference is [sim/docs/API.md](sim/docs/API.md).

## Language models

All model features go through one OpenAI-compatible client. Pick a backend:

```powershell
ollama pull llama3.1
.\run.ps1 -Server --port 8088 --ollama llama3.1       # Ollama on :11434, streaming off
```

or, for any other server (llama.cpp's `llama-server` defaults to :8080):

```powershell
$env:DND_LLM_URL   = "http://127.0.0.1:8080"
$env:DND_LLM_MODEL = "deepseek-r1"
.\run.ps1 -Server --port 8088
```

Other variables: `DND_LLM_PATH`, `DND_LLM_KEY`, `DND_LLM_TIMEOUT`, `DND_LLM_STREAM=0|1`. With a model configured:

- the web client's prompt box and `POST /api/games/:id/say` accept plain English;
- create a game with `"llmNarration": true` for prose scenes and `"llmDm": true` for a model-run Dungeon Master (CLI: `--llm-narration`, `--llm-dm`, `--llm-say`).

Reasoning models are supported: `reasoning_content` and `<think>` blocks are stripped and never shown. `GET /api/llm` reports the model's location in the store (`DEEPSEEK_MODEL_HOME`). `--mock-llm` runs the whole pipeline without a model, which is what the tests use.

## Layout

```
run.ps1, test.ps1   build and run, build and test
sim/
  data/           classes, weapons, spells, monsters, characters, modules/*.json   (README inside)
  include/dnd/    Dice, Types, Grid, Actor, Combat, AI, Human, DM, Content, Json, Game, Llm
  src/            implementation, main.cpp (terminal), server_main.cpp (HTTP)
  web/            single-page browser client                                       (README inside)
  tests/          tests.cpp (unit and integration), api_smoke.py (HTTP end to end) (README inside)
  third_party/    nlohmann/json, cpp-httplib (vendored, MIT)
  docs/API.md     HTTP reference
```

## Authoring content

Content is read from `sim/data/` (or `--data <dir>` / `DND_DATA`); games and saves are not stored there, see the data README. Add a file under `sim/data/modules/`, restart, and it appears in `GET /api/content`. The format is described in [sim/data/README.md](sim/data/README.md); `ravenmoor.json` uses every feature.

## Limitations

- Vampire Spawn's bite is simplified (no necrotic damage or maximum-HP drain).
- Spells are the ten in `spells.json`; classes have no subclasses or feats.
- Cover objects block movement and grant cover but not sight; only walls block sight.
- Dead characters stay dead (no resurrection); a total party kill ends the game.
- The LLM client speaks plain HTTP; put a proxy in front for TLS.
- The server holds games in memory; use the save endpoint to keep one.
