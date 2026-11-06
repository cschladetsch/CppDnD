# Web client

`index.html` is the whole client: HTML, CSS and vanilla JavaScript in one file, no build step and no dependencies. `dnd_server` serves it at `/`.

```powershell
.\r.ps1 -Server --port 8088        # then open http://127.0.0.1:8088/
```

## What it does

- Start screen: module (or a procedural delve), role (player, Dungeon Master, spectator), hero, optional seed, or load a save file.
- Exploration: exits with travel times, NPC topics, search, short and long rest, wait, pace, autopilot.
- Combat: the grid (click a square to move, click a creature to use the selected attack or spell), Dash, Dodge, potion, class features, End turn, and role swap at any time. Monster turns run automatically via `/advance`.
- Party and quest panels, an event log with flavour text, and Save, which downloads the game as JSON.
- Prompt box (plain English). Enabled when the server has a model: start it with `--ollama <model>` or set `DND_LLM_URL` and `DND_LLM_MODEL`.

## Using a different front end

The server only needs a folder of static files:

```powershell
.\r.ps1 -Server --static C:\path\to\my-client       # or set DND_WEB
```

Your client talks to the same `/api` routes; see [../docs/API.md](../docs/API.md). Pass `?since=<lastSeq>` on calls to receive only new events. CORS is open, so a client on another origin (a dev server, say) works too.

## Editing this one

Edit `index.html` and reload the page; the server reads it from disk on each request, so no rebuild is needed. The default folder is baked in at build time (`DND_WEB_DIR`), so run the server from the same checkout or pass `--static`.
