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

Two ways, same result: the server draws from the web client, or you run a script.

### From the game (Pictures panel)

`dnd_server` has a picture service. It builds a prompt for each entry in the JSON content and asks an image server for the picture, one at a time in the background, saving to `art/<kind>/<id>.png`. Open the **Pictures** panel in the web client:

- *Draw this scene*: the current place, the people there and the creatures in the current fight.
- *Party portraits*, *All missing*, and *Redo this scene* (overwrites).
- The model box switches the model (tick "from store" to demand that it exists in the model store).
- Progress with a Cancel button, and the last errors. A picture appears in the game as soon as it is saved.

Settings are in `data/images.json` (`--images FILE` or `DND_IMAGES`): `backend` (`a1111` for AUTOMATIC1111, Forge or SD.Next started with `--api`; `openai` for any server with `POST /v1/images/generations`), `url`, `checkpoint`, `requireStore`, `timeoutSec`. Wording, sizes and sampling are in `art/style.json`. To replace what one picture shows, put `"characters/thorin": "a dwarf in blackened plate, ..."` in `art/prompts.json` (keys are `kind/id`; the script reads the same file).

**The model store.** `checkpoint` names a folder in the CppLmmModelStore (`DEEPSEEK_MODEL_HOME`) that holds a `.safetensors` or `.ckpt`, for example `models/sd15/v1-5-pruned.safetensors`. The service resolves the name to that file and tells the image server to load it; the image server has to be able to see the file, so start AUTOMATIC1111 or Forge with `--ckpt-dir <model home>/sd15` (or put the file where it already looks). A name that is not in the store is passed through as it is, so a checkpoint the server already has still works; `requireStore: true` refuses those. As with language models, the store only locates and checks files; the image server does the drawing.

The HTTP routes are in [../docs/API.md](../docs/API.md#images).

### From a script

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
- To replace a generated subject, use `art/prompts.json` (see above) or add `"art": {"prompt": "..."}` to the entry in the JSON data.

Licence note: pictures you generate are governed by the licence of the checkpoint you used; check it before distributing them.
