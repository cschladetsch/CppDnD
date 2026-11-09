# CppDnD

A Dungeons & Dragons 5e simulator in C++20, built as a backend for a web game. One engine, three ways in: a terminal client (`dnd_sim`), an HTTP/JSON server (`dnd_server`) that also serves a browser client, and a plain-English prompt backed by a local model. You can play a party member or the Dungeon Master, swap roles mid-fight, or hand everything to the AI.

Design notes: [dungeons_dragons_c_simulator_architecture.md](dungeons_dragons_c_simulator_architecture.md)

## Quick start

```powershell
.\run.ps1                              # build if needed, then watch the AI play a 3 room delve
.\run.ps1 --interactive                # play it yourself (one hero, you choose each action)
.\run.ps1 --module ravenmoor --role player --hero elara --seed 7
.\web.ps1          # web client at http://127.0.0.1:8088/
.\test.ps1                           # build, then run every test
.\check.ps1                          # report what is missing (tools, files, port, Ollama); -Build, -Test, -Model
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
- **AI table.** Every actor can be its own mind: the DM, each kind of enemy (undead, beasts, ...), each party member and each NPC (by type) run on their own model, configured in one JSON file, and talk through JSON messages. Models can be switched mid-game. See [sim/data/README.md](sim/data/README.md).
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

Exploration commands: `look`, `go <place>`, `search`, `talk <npc> [topic]`, `rest short|long`, `wait <hours>`, `pace slow|normal|fast`, `party`, `hero [name]`, `quests`, `auto [n]`, `save <file>`, `quit`. In a fight, `help` lists the turn commands (`m`, `a`, `c`, `f rage|second_wind`, `p`, `dash`, `dodge`, `e`, `swap player|dm|auto`). Resume with `--load <file>`.

## Web server and client

```powershell
.\web.ps1
```

`web.ps1` checks for cmake, ninja and clang++, that the port is free, and (with `-Model`) that Ollama is running and has the model; builds if needed; starts `dnd_server`; waits until it answers; and opens the browser. Ctrl-C stops it. It also starts Stable Diffusion in the background for the Pictures panel (see below); if that is not installed yet the game starts anyway and says so. Options: `-Port`, `-Model`, `-NoArt` (do not start Stable Diffusion), `-Rebuild`, `-NoBrowser`; anything else is passed to `dnd_server`.

The client is at http://127.0.0.1:8088/ . You can also talk to the JSON API directly:

```powershell
curl -X POST localhost:8088/api/games -d '{"module":"ravenmoor","role":"player","hero":"elara","seed":7}'
```

Server flags: `--port`, `--host`, `--data <dir>`, `--static <dir>` (serve a different front end; also `DND_WEB`), `--llm-url`, `--llm-model`, `--ollama <model>`, `--agents <file>`, `--art <dir>`, `--images <file>`, `--history <dir>`, `--mock-llm`. The endpoint reference is [sim/docs/API.md](sim/docs/API.md).

## Language models

All model features go through one OpenAI-compatible client. Pick a backend:

```powershell
ollama pull llama3.1
.\web.ps1 -Model llama3.1          # Ollama on :11434, streaming off
```

or, for any other server (llama.cpp's `llama-server` defaults to :8080):

```powershell
$env:DND_LLM_URL   = "http://127.0.0.1:8080"
$env:DND_LLM_MODEL = "deepseek-r1"
.\web.ps1
```

Other variables: `DND_LLM_PATH`, `DND_LLM_KEY`, `DND_LLM_TIMEOUT`, `DND_LLM_STREAM=0|1`. With a model configured:

- the web client's prompt box and `POST /api/games/:id/say` accept plain English;
- create a game with `"llmNarration": true` for prose scenes and `"llmDm": true` for a model-run Dungeon Master (CLI: `--llm-narration`, `--llm-dm`, `--llm-say`).

Reasoning models are supported: `reasoning_content` and `<think>` blocks are stripped and never shown. `GET /api/llm` reports the model's location in the store (`DEEPSEEK_MODEL_HOME`). `--mock-llm` runs the whole pipeline without a model, which is what the tests use.

## AI table (a model per actor)

```powershell
.\check.ps1 -Model qwen2.5:7b     # is Ollama up, and are the models in sim\data\agents.json installed?
.\web.ps1 -Model qwen2.5:7b       # start, then tick "AI table" on the start screen
.\run.ps1 --agents                # terminal: same, using the default agents file
```

`sim/data/agents.json` says which model plays the DM, each creature type, each character and each NPC type, and what personality each has. In the web client the AI table panel lists them and lets you switch one mid-game; over HTTP it is `POST /api/games/:id/agents/model`. The CppLmmModelStore resolves and checks model names (`requireStore`, `GET /api/models`); the backend (Ollama, llama.cpp, vLLM) serves them. Details in [sim/data/README.md](sim/data/README.md) and [sim/docs/API.md](sim/docs/API.md).

## History

Everything said and done is recorded. Each actor has a private record of every other actor it dealt with, from its own side (`thorin-marta_hollin.json` is Thorin's view of Marta; `marta_hollin-thorin.json` is hers), and `group-history.json` holds what the whole table was told. The server writes them to `sim/history/<game id>/` (`--history DIR` to move them, `--history ""` to turn files off; the terminal client takes `--history DIR` too). The web client has a History panel, and AI-table agents are shown their own history in every prompt. Details in [sim/docs/API.md](sim/docs/API.md).

Switch which character you play at any time with the "You control" dropdown in the web client's Party panel, or `hero NAME` in the terminal.

## Art

### Setting up Stable Diffusion

Pictures need an image server. `sd.ps1` sets one up (AUTOMATIC1111, started with `--api --nowebui`) and `web.ps1` starts it beside the game by default:

```powershell
.\sd.ps1 -Check      # what is missing, changes nothing
.\sd.ps1 -Install    # git and Python 3.10 (winget, asks first), AUTOMATIC1111, and the SD 1.5 model into the model store
.\web.ps1            # game plus image server (-NoArt for the game alone); the first start installs PyTorch (10-20 minutes)
```

What it needs: Windows 10 or 11 with an NVIDIA GPU and driver (about 6 GB of VRAM or more is comfortable; less runs with `--medvram` or `--lowvram`; `-Cpu` works without a GPU but takes minutes per picture), git, Python 3.10 specifically (3.11 and newer do not work with AUTOMATIC1111), about 15 GB of free disk (PyTorch 5 GB, the repository, the 4.3 GB model), network access to github.com, huggingface.co, pypi.org and download.pytorch.org for the install, and port 7860. The model (Stable Diffusion 1.5, `v1-5-pruned-emaonly.safetensors`, CreativeML OpenRAIL-M licence) goes into the CppLmmModelStore at `<model home>/sd15/`, and AUTOMATIC1111 is pointed at that folder with `--ckpt-dir`, so the model is shared and downloaded once. Everything lives outside the repository: AUTOMATIC1111 in `%LOCALAPPDATA%\CppDnD\sd` (`-SdHome` or `DND_SD_HOME` to move it). `sd.ps1 -Start` and `-Stop` control the server by themselves; `web.ps1` stops it again when you stop the game, if it started it. With `-NoArt`, or before `sd.ps1 -Install` has been run, the game works as before and the Pictures panel says no image server is running. `.\check.ps1` reports the same requirements.

### Pictures

There is no bundled artwork, and none is needed. The web client draws its own tokens (a class or creature icon, a health ring, a pip per condition) and terrain tiles, and shows a picture wherever one exists in `sim/art/<kind>/<id>.png`. To make pictures from the game data with a local Stable Diffusion server (AUTOMATIC1111, Forge or SD.Next, started with `--api`), use the **Pictures** panel in the web client (the server draws in the background and the game picks each picture up as it is saved), or from a shell:

```powershell
.\art.ps1 -List                    # show every prompt, generate nothing
.\art.ps1                          # make whatever is missing
.\art.ps1 -Only characters -Force  # redo one kind
```

The model is chosen in `sim/data/images.json` and can be a folder in the CppLmmModelStore (`DEEPSEEK_MODEL_HOME`) holding a `.safetensors`; the panel can switch it mid-game. Or drop in your own files with the same names. Details in [sim/art/README.md](sim/art/README.md).

## Layout

Each folder has its own README.

```
run.ps1     build if needed, then run the terminal client (or the server with -Server)
web.ps1     build if needed, start the server and Stable Diffusion, open the browser (-NoArt: server only)
sd.ps1      check, install, start and stop Stable Diffusion (the image server)
art.ps1     draw every missing picture from a shell
test.ps1    build, then run every test
check.ps1   report what is missing: tools, files, content, port, Ollama
sim/                                                        engine, clients, content, tests
  data/          classes, weapons, spells, monsters, characters, modules/*.json
  include/dnd/   public headers (the engine's API)
  src/           implementation, main.cpp (terminal), server_main.cpp (HTTP)
  web/           single-page browser client
  history/       written at run time: per-game x-y.json and group-history.json (git-ignored)
  tests/         tests.cpp (unit and integration), api_smoke.py (HTTP end to end)
  docs/          API.md, the HTTP reference
  third_party/   nlohmann/json, cpp-httplib (vendored, MIT)
```

[sim/README.md](sim/README.md) explains how the pieces fit together.

## Authoring content

Content is read from `sim/data/` (or `--data <dir>` / `DND_DATA`); games and saves are not stored there, see the data README. Add a file under `sim/data/modules/`, restart, and it appears in `GET /api/content`. The format is described in [sim/data/README.md](sim/data/README.md); `ravenmoor.json` uses every feature.

## Limitations

- Vampire Spawn's bite is simplified (no necrotic damage or maximum-HP drain).
- Spells are the ten in `spells.json`; classes have no subclasses or feats.
- Cover objects block movement and grant cover but not sight; only walls block sight.
- Dead characters stay dead (no resurrection); a total party kill ends the game.
- The LLM client speaks plain HTTP; put a proxy in front for TLS.
- The server holds games in memory; use the save endpoint to keep one.
