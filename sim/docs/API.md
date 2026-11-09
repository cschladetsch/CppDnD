# HTTP API

All bodies are JSON. Errors are `{"error": "..."}` with a 4xx or 5xx status. CORS is open (`*`). Each game is serialized by its own lock, so requests to different games run in parallel.

Rules reference for the mechanics implemented here: the [D&D 5e SRD](https://5thsrd.org) and the [SRD API](https://www.dnd5eapi.co).

## Game lifecycle

| Method and path | Body | Result |
|---|---|---|
| `GET /api/health` | | `{"ok":true}` |
| `GET /api/content` | | modules, characters, monsters, spells, classes |
| `GET /api/llm` | | LLM client status and the model's location in the model store |
| `POST /api/games` | see below | `201 {"id", "view"}` |
| `GET /api/games` | | `{"games":[ids]}` |
| `GET /api/games/:id?since=N` | | the view, with only events after sequence `N` |
| `DELETE /api/games/:id` | | `{"deleted":true}` |
| `GET /api/games/:id/save` | | a complete save file |
| `POST /api/games/load` | a save file | `201 {"id", "view"}` (a new game) |

Create body (all optional): `module` (id, or empty for a procedural delve), `rooms`, `difficulty` (procedural), `party` (character ids), `level`, `seed`, `role` (`player`, `dm`, `spectator`), `hero`, `pace` (`slow`, `normal`, `fast`), `llmDm`, `llmNarration`.

## The view

```
mode          exploring | combat | complete | defeated
role, hero, pace, seed
module        {id, name, setting}
clock         {hour, clockHour, day, night, hoursSinceLongRest}
location      {id, name, kind, description, rest, exits[], npcs[]}
              exit: {to, name, label, miles, hours, available, blocked?}
              npc:  {id, name, description, topics[{id, heard}]}
travel        {to, hoursLeft}   only while a journey is interrupted by a fight
party[]       {id, name, class, level, xp, nextLevelXp, hp, maxHp, ac, dead, exhaustion,
               hitDice, hitDie, potions, slots, slotsMax, features[], spells[]}
quests[]      {id, name, description, done}
combat        null, or {round, finished, awaiting, current, grid{rows}, order, actors[],
               options?, result?}
events[]      {seq, type, actor, target, roll, vs, amount, text, flavour}
lastSeq       pass back as ?since= to receive only new events
```

`combat.awaiting` is the id of the actor whose human must answer (or `-1`). `combat.options` then lists that actor's economy, attacks, spells, features, potions and slots. Map glyphs: `#` wall, `~` rough, `c` half cover, `O` three-quarters cover.

## Exploration (valid when `mode` is `exploring`)

Each returns `{"ok", "message", "view"}`; `422` with `ok:false` when the command is refused. Any of them may start a fight (`mode` becomes `combat`).

| Path | Body |
|---|---|
| `POST /api/games/:id/travel` | `{"to": "greywick"}` |
| `POST /api/games/:id/search` | |
| `POST /api/games/:id/talk` | `{"npc": "marta", "topic": "rumours"}` (omit `topic` to list topics) |
| `POST /api/games/:id/rest` | `{"kind": "short" or "long", "hitDice": 99}` |
| `POST /api/games/:id/wait` | `{"hours": 1}` |
| `POST /api/games/:id/pace` | `{"pace": "fast"}` |
| `POST /api/games/:id/auto` | `{"steps": 1}` runs the autopilot (the party plays itself) |

Time rules: travel hours come from `miles` and pace (2, 3 or 4 mph) or the exit's `hours`; each hour on a risky site or road rolls the location's random table (slow pace halves the chance, fast raises it by half). Travel beyond 8 hours since the last long rest forces a Constitution save (DC 10 plus hours past 8) per member or one level of exhaustion. A long rest takes 8 hours, is allowed once per 24 hours, and is lost if interrupted. A short rest takes an hour and spends Hit Dice.

## Combat

| Path | Body | Meaning |
|---|---|---|
| `POST /api/games/:id/advance` | | run until a human must act or the fight ends |
| `POST /api/games/:id/action` | an action | answer the awaited actor, then run on |
| `POST /api/games/:id/hero` | `{"hero": "elara"}` (character id or name) | change which party member you control, at any time, even mid-turn; makes you a player if you were the DM or a spectator. 422 for an unknown or dead character. Returns `{view}`; `view.hero` is the name you control |
| `POST /api/games/:id/role` | `{"role": "dm", "hero": "elara"}` | change who plays what, at any time (`/hero` changes only the character) |

Actions:

```
{"type":"move","dest":[x,y]}
{"type":"attack","target":ID,"attack":INDEX}
{"type":"cast","spell":"fireball","target":ID,"slot":3}
{"type":"feature","feature":"rage"}          rage | second_wind
{"type":"potion","target":ID}
{"type":"dash"} {"type":"dodge"} {"type":"end_turn"}
{"type":"swap_role","role":"spectator"}
```

A refused action returns `422 {"ok":false,"reason":...}` and the same actor stays awaited.

## Typical client loop

```
create game
loop:
  GET view
  mode == exploring -> show location, send a command
  mode == combat    -> if combat.awaiting >= 0: render options, POST /action
                       else POST /advance
  mode == complete or defeated -> stop
```

## LLM

Set `DND_LLM_URL`, `DND_LLM_MODEL`, optionally `DND_LLM_PATH`, `DND_LLM_KEY`, `DND_LLM_TIMEOUT`, `DND_LLM_STREAM=0|1`, or pass `--llm-url` and `--llm-model`. Creating a game with `llmNarration` adds prose (`events[].flavour`) to scenes, falls, deaths, victories and quests. `llmDm` lets the model plan each monster turn; every action still goes through the rules engine and any failure hands the rest of that turn to the built-in AI.

## Web client

`dnd_server` serves a bundled single-page client from `sim/web/` at `/` (start screen, exploration, party and quests, combat grid with click to move and attack, role swap, save download and load). Use `--static <dir>` or the `DND_WEB` environment variable to serve a different front end; the `/api` routes are unaffected.

## Plain English

`POST /api/games/:id/say` with `{"text": "go to the village and ask Marta about rumours"}` asks the model to turn the text into the commands and actions above (up to 6 steps), then runs them through the normal rules. The first refused step stops the sequence. Returns `{ok, message, steps, view}`, where `message` is an in-character reply or the reason something was refused. A question that is not an action just gets a reply. Needs a configured model (`400` otherwise).

Using Ollama: `ollama pull llama3.1`, then `.\web.ps1 -Model llama3.1`. This sets `http://127.0.0.1:11434` with streaming off; any other OpenAI-compatible server works through `DND_LLM_URL` and `DND_LLM_MODEL`. The CLI has the same prompt with `--llm-say`: anything that is not a built-in command is sent to the model.

## World map

Every game view carries `map`: `{start, current, nodes: [{id, name, kind, visited, current}], edges: [{from, to, miles, hours, open}]}`. Nodes are the places the party has been plus the places their exits lead to, so it never reveals more than the next step. `open` is false while an exit's requirements are unmet. Layout is up to the client; the bundled one lays it out in columns by distance from `start`.

## History

Every game keeps a record of what was said and done, in two forms.

- **Pair history**, one private list per ordered pair, written from the owner's point of view. When x speaks to or acts on y, x's list about y holds it as `dir: "to"` and y's list about x holds it as `dir: "from"`. Nobody else's list sees it. Kinds: `ask`, `reply` (you and an NPC), `tell` (a private line between party members), `attack`, `spell`, `heal`, `down`, `death` (who did it to whom in a fight).
- **Group history**: what the whole table was told or said: `say`, `narrate`, `proposal` (the party's moves), `scene`, `travel`, `quest`, `combat`.

| Route | Returns |
|---|---|
| `GET /api/games/:id/history` | `{group, groupFile, dir, pairs: [{owner, with, count, file}]}` |
| `GET /api/games/:id/history/group?since=SEQ&limit=N` | `{entries: [{seq, day, hour, speaker, kind, text}]}` |
| `GET /api/games/:id/history/:x/:y?since=SEQ&limit=N` | `{owner, with, perspective, entries: [{seq, day, hour, speaker, dir, kind, text}]}`, x's private record of y (names match without regard to case; use the full name, `Marta%20Hollin`) |

The server also writes these as files, one per ordered pair and one for the group, in `<history dir>/<game id>/`: `thorin-marta_hollin.json` (Thorin's side), `marta_hollin-thorin.json` (Marta's side) and `group-history.json`. The folder defaults to `sim/history/` (`--history DIR` or `DND_HISTORY`; `--history ""` turns files off). History is also part of the save file, so a loaded game keeps it. Each list keeps its newest 2000 entries.

With agents on, an agent is shown a digest of its own history in every prompt (`history: {group: [...], with: {<name>: [...]}}`), and its reply may include `"tell": [{"to": "Elara", "text": "..."}]` for private lines to a companion. Those go to the pair histories and are not shown in the public log.

## Agents (every actor its own model)

Create a game with `"agents": true` (needs an agents file, see [../data/README.md](../data/README.md)). The Dungeon Master, each kind of enemy, each party member and each NPC then think with their own model and persona. Agents exchange JSON through a shared table: each turn an agent receives `{task, you, role, table, your_recent_decisions, observation, reply_schema}` and answers with one JSON object, for example `{"say": "...", "actions": [...]}` in a fight or `{"say": "...", "propose": {"cmd": "travel", "to": "greywick"}}` when the party decides where to go (the most-proposed move wins; the first speaker breaks ties). Words spoken appear in the event log; an NPC speaks its topic in its own voice and falls back to the written line if its model fails. The rules engine validates every action, and anything illegal or unparseable is played by the built-in AI instead.

| Path | Meaning |
|---|---|
| `GET /api/games/:id/agents?since=N` | `{enabled, agents: [{name, role, selector, backend, url, model, persona, calls, failures, modelStore}], problems, messages, lastSeq}`; `messages` are the JSON exchanged at the table (`say`, `narrate`, `proposal`, `decision`) |
| `POST /api/games/:id/agents/model` | switch a model mid-game: `{"selector": "type:undead", "model": "phi3:mini", "backend": "ollama", "fromStore": false}`; `fromStore: true` requires the model to exist in the model store |
| `GET /api/models` | models present in the CppLmmModelStore |
| `GET /api/art` | `{enabled, base, characters, monsters, npcs, locations, modules}`, each a map of id to a path under `base` (with `?v=` for cache busting); only ids that have a picture are listed. Files are served under `/art/`. |
| `GET /api/llm` | also reports `agents: {available, file, error}` |

Selectors: `dm`, `player:<name>`, `npc:<id>`, `type:<creature type>`, `monster:<id>`. Switching `type:undead` changes every zombie, skeleton, ghoul, specter and vampire spawn at once, now and for ones that appear later. The model store resolves and checks model names; serving the model is the backend's job (an Ollama tag, a llama.cpp router, vLLM).

Fights with agents block while models think; expect `advance` and `auto` to take as long as the slowest model (`timeoutSec` per backend).
NPCs in a location's view now carry a `type`.
