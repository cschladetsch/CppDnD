# Content

Everything the engine knows about the game lives here as JSON. Files may contain `/* */` and `//` comments. Content is loaded at startup from this folder (override with `--data <dir>` or `DND_DATA`) and validated; an error names the file and field.

| File | Holds |
|---|---|
| `classes.json` | spell slot tables (`full`, `half`) and per-class progression: hit die, proficiency, Extra Attack, features |
| `weapons.json` | `id`, `name`, `damage`, `type`, optional `range: [normal, long]`, `ability`, `finesse` |
| `spells.json` | `id`, `name`, `level`, `range`, `kind` (`attack`, `saveNegates`, `saveHalf`, `autoHit`, ...), `dice`, `damageType`, plus `save`, `projectiles`, `concentration` and similar |
| `monsters.json` | SRD stat blocks: `cr`, `xp`, `ac`, `hp`, `speed`, `scores` (STR to CHA), `tags`, `attacks`, optional `resist`, `immune`, `vulnerable`, `resistNonmagical` |
| `characters.json` | the classic party (used by the procedural delve): `class`, `level`, `scores`, `ac`, `speed`, `weapons`, `spells`, `potions` |
| `modules/<id>/characters.json` | that module's own cast, same schema, ids unique across all files. A new game of the module starts with its `party` (optional list of ids in the module file) or else the first four of its cast; choosing another cast member as hero swaps them into the last seat |
| `agents.json` | which model thinks for which actor (see "agents.json" below) |
| `modules/*.json` | one file per module (see below) |

## Where content is read from

All content is loaded from one directory, chosen in this order:

1. `--data <dir>` on `dnd_sim` or `dnd_server`
2. the `DND_DATA` environment variable
3. the folder this one was built from (`sim/data`, baked in at build time as `DND_DATA_DIR`; an installed build uses `share/dnd/data`)

The server prints the directory it chose as `data=...` in its startup line. The directory must contain `classes.json`, `weapons.json`, `spells.json`, `monsters.json`, `characters.json` and a `modules/` folder; every `modules/*.json` is a module, and its `id` is what you pass as `--module` or `"module"` in the API. Content is read once at startup, so restart after editing.

To keep your own modules out of the repo, copy this folder, edit the copy, and point `--data` or `DND_DATA` at it.

## Not stored here: games and saves

Content is read-only. A running game is held in the server's memory and is gone when the server stops. A save is a separate JSON file (the whole state including the RNG stream): the web client's Save button downloads it, `GET /api/games/:id/save` returns it, and the terminal client writes `save.json` or the path given with `--save`. Load one with `--load <file>`, `POST /api/games/load`, or the start screen's file picker.

Dice are written `2d6+3`. Ability scores are `[STR, DEX, CON, INT, WIS, CHA]`. Monster stats follow SRD 5.1 unless a comment says a creature is original (`cult_adept`, `hollow_king`, `pale_abbess`, and the CyberHood creatures `gutter_punk` to `hush_avatar`; creature tag `construct` is new and has its own persona in `agents.json`).

## Adding a monster, spell or character

Append an object to the array in the matching file, using an existing entry as the template, then run `.\test.ps1`. The content test loads every file, so a typo fails there. Refer to it by `id` from modules, `--party`, or the API.

## Modules

A module is the space for a long session. Top-level keys:

| Key | Meaning |
|---|---|
| `id`, `name`, `setting`, `intro`, `endText` | identity and framing text |
| `recommendedLevel` | shown in the client; autopilot is tuned for it |
| `hardHp` | monster hit point scale on Hard (default 1.25); tune so an AI-only party wins about one game in three |
| `clock` | `{startHour, nightStart, nightEnd}`; night raises random encounter chances |
| `start` | id of the starting location |
| `locations` | the graph (below) |
| `tables` | random encounter tables: `chanceDay`, `chanceNight` (percent per risky hour) and weighted `entries` of `monsters` |
| `draws` | seeded secrets: `{id, choices}`; the pick is stored as flag `<id>=<choice>` |
| `quests` | `{id, name, description, completeWhen: [flags], xp}` |
| `endWhen` | flag that completes the module |
| `art` | optional picture style for this module: `prefix`, `suffix`, `negative`, `creature` (wording for monsters), `framing` (per kind). Replaces the global `art/style.json` words for the module's pictures |
| `roomSeeds` | optional `[{name, text}]`: rooms that Explore finds when no model writes them, so a setting can have rooms of its own (the general fantasy ones are used when empty) |

A location has `id`, `name`, `kind` (`wilderness`, `settlement`, `boundary`, `site`), `description`, `rest` (`safe`, `risky`, `none`), an optional `randomTable` and `map`, and lists of `events`, `exits` and `npcs`.

- **exits:** `{to, label, miles | hours, requires: [flags], blockedText}`. Travel time comes from `miles` and pace (2, 3 or 4 mph) or the fixed `hours`.
- **events:** `{id, trigger: "enter" | "search", narration, once, requires: [flags], setsFlags: [flags], xp, potions, encounter: {monsters: [ids]}}`. An event with an `encounter` starts a fight; `setsFlags` and `xp` apply when it is won.
- **npcs:** `{id, name, type, description, topics: [{id, text, setsFlags}], chatter: [lines]}`; hearing a topic sets its flags. `chatter` is what the NPC says to another NPC in the same place when no model writes the line: when the party arrives, rests or waits, one NPC in a place with two or more speaks to one of the others, in rotation. Whoever speaks and whoever hears keep the line in their own private histories, along with the first sight of the party and of each other, so every NPC has its own record of everyone it has dealt with (`GET /history/<name>`). `type` classifies the NPC (`innkeeper`, `villain`, `merchant`, `priest`, `guard`, ... any word; default `commoner`) and is what `agents.json` uses to pick a model.
- **map:** the tactical grid for fights at that location: `.` floor, `#` wall, `~` rough, `c` half cover, `O` three-quarters cover, `P` party start, `M` monster spawn.

`ravenmoor.json` demonstrates every one of these. `cyberhood.json` is the larger example: 11 locations, five quests, three random tables, a draw (`override=canopy|undergrid`) that decides where the key is hidden, events gated by `!flag` so that the same scene plays differently with or without a stolen pass, and two routes into the corporate plaza. `hollow_crown.json` is a 20-location module: 20 locations, eight quests, four random tables, two independent draws (`seal=chapel|mill` hides the necropolis gate seal, `spy=perrin|odo` decides which innkeeper or clerk is the cult's informant), two ways over the pass (the Broken Bridge, or the hunters' trail with a guide) and two ways into the lakeside town (the orc fort or the smugglers' tunnel). `sunless_abbey.json` is the largest: 50 locations in five tiers (the village and approach, the cloister, the undercroft and lamp-mine, the ossuary, the sanctum), seven quests, five random tables, two independent draws (`key=scriptorium|infirmary` hides the cellar key, `thrall=bess|tolliver` decides which villager serves the abbess) and three pairs of alternate routes (the gate or the orchard breach into the abbey; the cellar key or the goblin track into the undercroft; the crypt word or the ore lift into the ossuary). Almost every room is quiet until you act: about thirty rooms hold a scripted fight, and a room whose fight is behind you is marked `"rest": "safe"`, because a 50-room module gives a party too few chances to long-rest otherwise. Tier crossings cost four hours (`"hours": 4`) so that the 24-hour long-rest rule comes round. Its boss, `pale_abbess`, is an original creature in `monsters.json`. `tests/test_modules.cpp` checks every module: connected, no dead ends, every gating flag can be earned for every outcome of the draws, and the autopilot finishes. `meridian.json` is a second 50-room module, set aboard the colony ship Meridian: five decks (the docking ring, the habitat ring, the science and hydroponics decks, engineering, command), an alien swarm in the creatures `skitterling`, `husk`, `brood_drone`, `acid_spitter`, `stalker`, `brood_warrior` and the boss `hive_queen` (all original, tagged `alien`), and the ship's own `security_mech`. Two draws (`keycard=medbay|officers` hides the chief engineer's card, `host=pip|teo` decides which survivor carries an embryo), two ways into engineering (the keycard door, or the vent shaft once the grate is pried loose), two ways into command (the lift once the reactor is back up, or across the hull in a vacuum suit), and a nest that opens only with power and knowledge of where it is. Thirteen NPCs share six rooms between them and speak to one another through `chatter`; `roomSeeds` gives Explore ship rooms to find. Crossings between decks cost three hours, which matters: more travel between long rests than that tires the party out.

Travel time: an exit with `miles` or `hours` costs at least one hour (rounded up); an exit with neither takes no time, which suits rooms in one complex (a stair down to a crypt, the other side of a town square). After 8 travel hours without a long rest, each further hour is a forced march (CON saves, then exhaustion), and the autopilot long-rests only when the party is hurt, so a module made only of one-hour hops will exhaust an AI party long before the finale.

## images.json

Where pictures are drawn (see [../art/README.md](../art/README.md)): `enabled`, `backend` (`a1111` or `openai`), `url`, `path` (openai only), `apiKey`, `checkpoint` (a folder name in the CppLmmModelStore, or a name the image server knows), `requireStore`, `timeoutSec`. Comments are allowed. Missing file: defaults (AUTOMATIC1111 on 127.0.0.1:7860, whatever model it has loaded).

## agents.json

Used by games created with agents on (`"agents": true`, or `--agents`). Every actor becomes its own mind with its own model, persona and memory, and they talk to each other through JSON messages (see [../docs/API.md](../docs/API.md)). Comments are allowed. The shipped file names Ollama models; edit it to match what you have installed (`.\check.ps1` lists any that are missing).

```json
{
  "backends": { "ollama": { "url": "http://127.0.0.1:11434", "timeoutSec": 120 } },
  "defaults": { "backend": "ollama", "model": "qwen2.5:7b", "temperature": 0.7, "maxTokens": 400 },
  "dm":       { "persona": "..." },
  "monsterTypes": { "undead": { "model": "dolphin-llama3:8b", "persona": "..." }, "beast": { "model": "gemma2:2b" } },
  "monsters":     { "vampire_spawn": { "model": "qwen2.5:7b" } },
  "players":      { "*": {}, "thorin": { "model": "dolphin-llama3:8b", "persona": "..." } },
  "npcTypes":     { "*": { "model": "gemma2:2b" }, "innkeeper": { "model": "phi3:mini" } },
  "npcs":         { "marta": { "persona": "..." } }
}
```

| Section | Who | Inherits, later wins |
|---|---|---|
| `dm` | the Dungeon Master: narration and any monster with no entry of its own | `defaults`, `dm` |
| `monsterTypes` | every enemy of a creature type (the `tags` in `monsters.json`: `undead`, `beast`, `humanoid`, `giant`) shares one mind | `dm`, `monsterTypes["*"]`, `monsterTypes[type]` |
| `monsters` | one kind of monster by id (`goblin`, `vampire_spawn`, ...) | the above, then `monsters[id]` |
| `players` | each party member, by character id or name | `defaults`, `players["*"]`, `players[id]` |
| `npcTypes`, `npcs` | module NPCs, by their `type`, then by id or name | `defaults`, `npcTypes["*"]`, `npcTypes[type]`, `npcs[id]` |

Each entry may set `backend`, `model`, `persona`, `temperature`, `maxTokens`, `enabled` and `requireStore`. `"enabled": false` (or, for an NPC, no model) hands that actor back to the built-in AI or the written dialogue. `"requireStore": true` demands that the model exists in the CppLmmModelStore (`DEEPSEEK_MODEL_HOME`); if not, that actor falls back to the built-in AI and the problem is reported by `GET /api/games/:id/agents`. A backend entry is any OpenAI-compatible server: `url`, `path`, `apiKey`, `stream`, `timeoutSec`. Set `"format": "cppcoder"` to talk to [CppLocalLlmCodeAssist](https://github.com/cschladetsch/CppLocalLlmCodeAssist) instead (`cppcoder --serve`, default `http://127.0.0.1:8765`; `path` defaults to `/api/chat`; it streams Ollama NDJSON and swaps models per request, so each actor's `model` is whatever Ollama has pulled). Example: `"backends": { "cppcoder": { "url": "http://127.0.0.1:8765", "format": "cppcoder" } }`, then `"defaults": { "backend": "cppcoder" }`. That server prepends its remembered facts to every chat and extracts new ones from the prompt, so point it at a separate `--memory-file` for the game. `DND_LLM_FORMAT=cppcoder` does the same for the single-model env setup.

The file is found at `--agents <file>`, else `DND_AGENTS`, else `agents.json` in the data folder. A model can also be switched while a game runs: `POST /api/games/:id/agents/model`.

Local models: running different models for many actors means Ollama loads and unloads them. Set `OLLAMA_MAX_LOADED_MODELS` (and mind your VRAM) or give several actors the same model, otherwise each turn pays a load. A dead or slow model never stalls a game: after repeated failures that actor is skipped for a while and the built-in AI plays.
