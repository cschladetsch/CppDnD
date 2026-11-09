# Web client

`index.html` is the whole client: HTML, CSS and vanilla JavaScript in one file, no build step and no dependencies. `dnd_server` serves it at `/`.

```powershell
.\web.ps1                         # checks and sets up cmake/ninja/clang++, Ollama and its models, Stable Diffusion; builds; starts the server; opens the browser
```

## What it does

- Start screen: module (or a procedural delve), role (player, Dungeon Master, spectator), hero, optional seed, or load a save file.
- World map, shown in every state (exploring, fighting, complete, defeated): the places visited and the places they lead to, with travel times, your position highlighted (red while fighting), and click-to-travel on open exits. Nothing beyond the next step is revealed.
- Exploration: exits with travel times, NPC topics, search, short and long rest, wait, pace, autopilot.
- Combat: the grid (click a square to move, click a creature to use the selected attack or spell), Dash, Dodge, potion, class features, End turn, and role swap at any time. Monster turns run automatically via `/advance`.
- "You control" dropdown in the Party panel: switch which party member you play at any time (the current one is marked). Dead characters are greyed out.
- History panel: the group chat, or any actor's private record of another (for example Elara's record of Marta), updated as the game runs.
- Party and quest panels, an event log with flavour text, and Save, which downloads the game as JSON.
- AI table panel (when a game uses agents): every actor with its model and call counts, and a control to switch any actor or enemy type to another model mid-game. The start screen has an "AI table" checkbox when an agents file is found.
- Pictures: combat uses drawn SVG tokens (class or creature icon, health ring, condition pips) on tiled terrain; any picture found in the art folder (`GET /api/art`) replaces the token, the location text gets a backdrop, NPCs and party members get portraits. See [../art/README.md](../art/README.md).
- Main screen: a gold button draws the scene through the controlled hero's eyes; **Camera...** opens a live 3D preview you can fly around in (WASD, QE, arrows, mouse drag, wheel) and draw from, using the preview as the starting picture.
- Sessions: the header dropdown lists every game the server keeps (name, hero, day, mode), switches between them without losing any state, and opens "+ New session..." for the start screen. Rename and Delete act on the open session. The last open session is reopened when the page loads.
- Gallery panel: every stored picture in a grid, newest first, with a full-size view on click.
- Pictures panel: draw the scene, the party portraits or everything missing with the local image model, switch the model, watch progress.
- Prompt box (plain English). Enabled when the server has a model: start it with `--ollama <model>` or set `DND_LLM_URL` and `DND_LLM_MODEL`.

## Using a different front end

The server only needs a folder of static files:

```powershell
.\web.ps1 --static .\my-client          # or set DND_WEB
```

Your client talks to the same `/api` routes; see [../docs/API.md](../docs/API.md). Pass `?since=<lastSeq>` on calls to receive only new events. CORS is open, so a client on another origin (a dev server, say) works too.

## Editing this one

Edit `index.html` and reload the page; the server reads it from disk on each request, so no rebuild is needed. The default folder is baked in at build time (`DND_WEB_DIR`), so run the server from the same checkout or pass `--static`.
