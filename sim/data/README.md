# Content

Everything the engine knows about the game lives here as JSON. Files may contain `/* */` and `//` comments. Content is loaded at startup from this folder (override with `--data <dir>` or `DND_DATA`) and validated; an error names the file and field.

| File | Holds |
|---|---|
| `classes.json` | spell slot tables (`full`, `half`) and per-class progression: hit die, proficiency, Extra Attack, features |
| `weapons.json` | `id`, `name`, `damage`, `type`, optional `range: [normal, long]`, `ability`, `finesse` |
| `spells.json` | `id`, `name`, `level`, `range`, `kind` (`attack`, `saveNegates`, `saveHalf`, `autoHit`, ...), `dice`, `damageType`, plus `save`, `projectiles`, `concentration` and similar |
| `monsters.json` | SRD stat blocks: `cr`, `xp`, `ac`, `hp`, `speed`, `scores` (STR to CHA), `tags`, `attacks`, optional `resist`, `immune`, `vulnerable`, `resistNonmagical` |
| `characters.json` | party members: `class`, `level`, `scores`, `ac`, `speed`, `weapons`, `spells`, `potions` |
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

Dice are written `2d6+3`. Ability scores are `[STR, DEX, CON, INT, WIS, CHA]`. Monster stats follow SRD 5.1 unless a comment says a creature is original.

## Adding a monster, spell or character

Append an object to the array in the matching file, using an existing entry as the template, then run `.\test.ps1`. The content test loads every file, so a typo fails there. Refer to it by `id` from modules, `--party`, or the API.

## Modules

A module is the space for a long session. Top-level keys:

| Key | Meaning |
|---|---|
| `id`, `name`, `setting`, `intro`, `endText` | identity and framing text |
| `recommendedLevel` | shown in the client; autopilot is tuned for it |
| `clock` | `{startHour, nightStart, nightEnd}`; night raises random encounter chances |
| `start` | id of the starting location |
| `locations` | the graph (below) |
| `tables` | random encounter tables: `chanceDay`, `chanceNight` (percent per risky hour) and weighted `entries` of `monsters` |
| `draws` | seeded secrets: `{id, choices}`; the pick is stored as flag `<id>=<choice>` |
| `quests` | `{id, name, description, completeWhen: [flags], xp}` |
| `endWhen` | flag that completes the module |

A location has `id`, `name`, `kind` (`wilderness`, `settlement`, `boundary`, `site`), `description`, `rest` (`safe`, `risky`, `none`), an optional `randomTable` and `map`, and lists of `events`, `exits` and `npcs`.

- **exits:** `{to, label, miles | hours, requires: [flags], blockedText}`. Travel time comes from `miles` and pace (2, 3 or 4 mph) or the fixed `hours`.
- **events:** `{id, trigger: "enter" | "search", narration, once, requires: [flags], setsFlags: [flags], xp, potions, encounter: {monsters: [ids]}}`. An event with an `encounter` starts a fight; `setsFlags` and `xp` apply when it is won.
- **npcs:** `{id, name, description, topics: [{id, text, setsFlags}]}`; hearing a topic sets its flags.
- **map:** the tactical grid for fights at that location: `.` floor, `#` wall, `~` rough, `c` half cover, `O` three-quarters cover, `P` party start, `M` monster spawn.

`ravenmoor.json` demonstrates every one of these.
