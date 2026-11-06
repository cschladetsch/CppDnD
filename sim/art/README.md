# art

Optional pictures. The game runs and looks complete without any; the web client draws its own tokens and terrain.

## Layout

```
art/
  style.json       prompt style for the generator
  characters/<id>.png   party members (id from data/characters.json), shown in the party panel and on combat tokens
  monsters/<id>.png     creatures (id from data/monsters.json), shown on combat tokens
  npcs/<id>.png         NPCs (id from a module), shown next to their topics
  locations/<id>.png    places (id from a module), shown as a backdrop above the scene text
  modules/<id>.png      a module banner, used when the current place has no picture
```

Accepted extensions: png, jpg, jpeg, webp, svg. Wide pictures (locations, modules) suit about 16:9; portraits and tokens are shown square.

The server serves this folder at `/art/` and lists what exists at `GET /api/art`. Choose another folder with `--art DIR` or `DND_ART`. Add or replace a file and reload the page; no restart needed.

## Generating pictures

`tools/gen_art.py` (wrapped by `..\art.ps1`) reads the JSON content, builds a prompt for each entry, and asks a local AUTOMATIC1111-compatible server (`POST /sdapi/v1/txt2img`, also Forge and SD.Next, started with `--api`) for a picture.

```powershell
.\art.ps1 -List                          # print every prompt and size, generate nothing
.\art.ps1                                # generate what is missing
.\art.ps1 -Only monsters,npcs            # some kinds
.\art.ps1 -Ids thorin,zombie -Force      # some ids, overwriting
.\art.ps1 -Url http://192.168.1.20:7860  # another machine ($env:SD_URL also works)
```

- Each picture uses a seed derived from its id, so the same prompt gives the same picture.
- Existing files are skipped unless forced, so an interrupted run resumes.
- `style.json` holds the style words (`prefix`, `suffix`, `negative`), `steps`, `cfg`, `sampler`, `checkpoint` (empty keeps the loaded one) and the two sizes.
- To replace a generated subject, add `"art": {"prompt": "..."}` to a character, monster, NPC, location or module in the JSON data.

Licence note: pictures you generate are governed by the licence of the checkpoint you used; check it before distributing them.
