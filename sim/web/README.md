# Web client

`index.html` is the whole client: HTML, CSS and vanilla JavaScript in one file, no build step and no dependencies. `dnd_server` serves it at `/`.

```powershell
.\web.ps1                         # checks and sets up cmake/ninja/clang++, Ollama and its models, Stable Diffusion; builds; starts the server; opens the browser
```

## What it does

- Start screen: module (or a procedural delve), role (player, Dungeon Master, spectator), hero (the chosen module's own cast; the procedural delve offers the classic four), optional seed, or load a save file.
- World map, shown in every state (exploring, fighting, complete, defeated): a painted top-down chart (terrain, roads, and a drawing of each village, wood, ruin or gate) of the places visited and the places they lead to, with travel times, fog over places not yet seen, your party's standard on its place (red while fighting), and click-to-travel on ringed places. Drawn with canvas 2D, so it needs no image model. Nothing beyond the next step is revealed.
- Exploration: exits with travel times, NPC topics, a "Memory" button per NPC (what that person remembers of the party and of the other NPCs, from `GET /history/<name>`), search, short and long rest, wait, pace, autopilot.
- Combat: the grid (click a square to move, click a creature to use the selected attack or spell), Dash, Dodge, potion, class features, End turn, and role swap at any time. Monster turns run automatically via `/advance`.
- "You control" dropdown in the Party panel: switch which party member you play at any time (the current one is marked). Dead characters are greyed out.
- History panel: the group chat, or any actor's private record of another (for example Elara's record of Marta), updated as the game runs.
- Equipment panel: a common space plus one box per character, each showing its items as pictures (a drawn icon for every weapon and potion, or the generated picture from the "Item pictures" button when one exists). Drag an item onto a character or the common space, drop it on one of their items to swap, or click an item and then click where it goes. Party rows show a strip of each character's gear. Only while exploring.
- Party and quest panels, an event log with flavour text, and Save, which downloads the game as JSON.
- AI table panel (when a game uses agents): every actor with its model and call counts, and a control to switch any actor or enemy type to another model mid-game. The start screen has an "AI table" checkbox when an agents file is found.
- Pictures: combat uses drawn SVG tokens (class or creature icon, health ring, condition pips) on tiled terrain; any picture found in the art folder (`GET /api/art`) replaces the token, the location text gets a backdrop, NPCs and party members get portraits. See [../art/README.md](../art/README.md).
- **Walk...** moves you through the scene Street View style. Step with W/A/S/D or the arrow keys (Shift for two steps), turn with Q/E, look up and down with R/F, click the floor to walk to that spot, drag the picture to look around, and use the wheel to step forward and back. Walls, obstacles and people block the way, and a plan view shows where you are. The image model draws a picture at each place you stop (one seed per location keeps the stops looking like the same room); while it works the last picture is slid and zoomed toward where you went, so movement feels immediate. "Pre-draw nearby stops" quietly draws one step ahead, behind and either side while you stand still, and "Where you have been" keeps a strip of thumbnails you can click to go back. Needs picture generation (Stable Diffusion); without it you still get the live wireframe.
- Main screen: a gold button draws the scene through the controlled hero's eyes; **Camera...** opens a live 3D preview you can fly around in (WASD, QE, arrows, mouse drag, wheel) and draw from, using the preview as the starting picture.
- Sessions: the header dropdown lists every game the server keeps (name, hero, day, mode), switches between them without losing any state, and opens "+ New session..." for the start screen. Rename and Delete act on the open session. The last open session is reopened when the page loads.
- Gallery panel: every stored picture in a grid, newest first, with a full-size view on click.
- Pictures panel: draw the scene, the party portraits or everything missing with the local image model, switch the model, watch progress.
- Prompt box (plain English). Enabled when the server has a model: start it with `--ollama <model>` or set `DND_LLM_URL` and `DND_LLM_MODEL`.
- **Play aids.** In a fight the squares you can reach are shaded and every foe shows your chance to hit and the expected damage (hover for cover, advantage and range). The log gives one plain line per attack; click it for the dice. Chips under the prompt suggest things to say. **Undo** takes back your last action, **Rewind** returns to the last rest, **Journal** lists quests and the people you have met, and a "Previously..." line opens a returning game. Hover a condition for its rule.
- **Settings** (saved in the browser): sound effects and a quiet drone per module (synthesised, no files), narration and NPC lines read aloud by the device's own voices, typed-out narration (click to finish it), computer turns without a pause, text size and a colour-blind marker set. Neighbouring rooms' pictures are fetched ahead of time.
- **Tips.** Hover or focus anything with help (buttons, squares, the turn order, each attack line in the log) for a styled popover; every `title=` in the page becomes one. Coach cards explain things the first time they happen: initiative, your turn's budget, the chance to hit, how a roll was read (`rolled 14 on the d20, +5, total 19, against AC 15`), advantage, cover, range, opportunity attacks, saving throws, concentration, conditions, going down and death saves, victory. Each has **Close** (back after the next page load) and **Do not show again** (gone for good). While one is open the computer's turns wait. Settings switches them off or shows them all again. Dismissals are kept in the browser under `cppdnd.tips`.
- **Combat tutorial.** "Learn combat" on the start screen opens the Training Yard (`data/modules/tutorial.json`) with a card per step: into the ring, the turn order, moving, attacking, reading the result, ending the turn, the others' turns, throwing and cover, spells, staying alive, finishing the fight. Each step moves on when the game shows it was done, and none blocks what you can do. It can be skipped, and reopening the session resumes it.
- **Keys.** `/` focuses the prompt, `1` to `9` choose an attack or spell, `E` ends the turn, `U` undoes, `J` journal, `S` settings, `?` help; Tab and Enter work on squares and buttons. New games default to Normal difficulty.

## 3D models viewer

`models.html` (the **3D** button in the header) shows each world's textures and its characters as real 3D models and lets you fly through the room: click the view, then W A S D to move, the mouse to look, Q and E down and up, Shift for speed, the wheel to change speed, Esc to free the mouse (arrow keys also turn). Click a character to pick it, F flies to it, and Shift+click the floor sends it there. It uses a vendored three.js (`vendor/three`, MIT, no network needed).

Everything it shows is built by Blender, from the game data, with `assets.ps1` at the repo root (`sim/tools/gen_assets.py`, run as `blender -b -P`, or with the `bpy` pip module):

- **Textures.** Each world has a floor and a wall made from shader nodes (cobble, flagstone, brick, riveted plate, moss) and baked to a diffuse and a normal map; noise-based ones are made seamless. `.\assets.ps1 -NewTextures` adds another variant of every world (new noise seed, shifted colours) and keeps the old ones. A running viewer notices the new set within ten seconds and switches to it.
- **Characters.** One rigged low-poly model per hero (the global cast and each module's cast) with Idle and Walk animations and its own baked cloth texture. Look, build and props come from the id and class, so a hero is always the same figure, and an existing model is never overwritten unless `-RebuildCharacters` is given. Sci-fi worlds get helmets and blasters instead of hats and swords.
- **Persistence.** The files are the persistent part (`assets/manifest.json` lists them). The viewer also remembers your world, texture variant, animation, camera and where you put each character, in the browser.

The files under `assets/` are generated and not committed: run `.\assets.ps1` once (it needs Blender, `winget install BlenderFoundation.Blender`, and takes under a minute) and the viewer has everything. Until then it says there are no assets yet.

## Using a different front end

The server only needs a folder of static files:

```powershell
.\web.ps1 --static .\my-client          # or set DND_WEB
```

Your client talks to the same `/api` routes; see [../docs/API.md](../docs/API.md). Pass `?since=<lastSeq>` on calls to receive only new events. CORS is open, so a client on another origin (a dev server, say) works too.

## Editing this one

Edit `index.html` and reload the page; the server reads it from disk on each request, so no rebuild is needed. The default folder is baked in at build time (`DND_WEB_DIR`), so run the server from the same checkout or pass `--static`.

## Adding a tip

Tips live in `index.html`, in the section that starts `// ---- tips`. Call `coachOffer(id, title, body, anchor)` from `coachOnEvent(e)` (an event from the engine; attack and save events carry `check`, `natural`, `mode`, `crit`, `coverBonus` and `opportunity`, and `explainRoll(e)` turns one into a sentence) or from `coachOnView(v)` (what is on screen). `id` is what "Do not show again" remembers, `body` is a string or a function, and `anchor` is an optional function returning the element to outline. A tip is offered once per page load and only for events that happen live, never for a log replayed when a session is opened. Tutorial steps are the `TUT` array: `{title, body, anchor, done(view, event), next, hold}`.
