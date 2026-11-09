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

Settings are in `data/images.json` (`--images FILE` or `DND_IMAGES`): `backend` (`a1111` for AUTOMATIC1111, Forge or SD.Next started with `--api`; `openai` for any server with `POST /v1/images/generations`), `url`, `checkpoint`, `requireStore`, `timeoutSec`. Wording, sizes and sampling are in `art/style.json`.

**Camera.** A Stable Diffusion model has no camera, only the text prompt and the picture's shape, so framing is worded into the prompt. Each picture is `prefix, subject, framing, suffix`: the subject comes from your JSON content, and the framing comes from the `framing` map in `style.json`, one phrase per kind. The defaults are an eye-level, shallow-depth upper-body portrait for characters and NPCs, a centered three-quarter full-body view on a plain dark background for monsters (so they read as tokens), an eye-level wide establishing shot for locations, and a low-angle panorama for module banners. Pictures are 512x512 for people and creatures and 768x432 (16:9) for places; change the `portrait` and `wide` sizes to change the aspect. A prompt from `prompts.json` is used exactly as written, with no framing added, so put the shot in it yourself. To replace what one picture shows, put `"characters/thorin": "a dwarf in blackened plate, ..."` in `art/prompts.json` (keys are `kind/id`; the script reads the same file).

**The model store.** `checkpoint` names a folder in the CppLmmModelStore (`DEEPSEEK_MODEL_HOME`) that holds a `.safetensors` or `.ckpt`, for example `models/sd15/v1-5-pruned.safetensors`. The service resolves the name to that file and tells the image server to load it; the image server has to be able to see the file, so start AUTOMATIC1111 or Forge with `--ckpt-dir <model home>/sd15` (or put the file where it already looks). A name that is not in the store is passed through as it is, so a checkpoint the server already has still works; `requireStore: true` refuses those. As with language models, the store only locates and checks files; the image server does the drawing.

The HTTP routes are in [../docs/API.md](../docs/API.md#images).

### Views from a camera

`POST /api/games/:id/viewpoint` (the main-screen button and the camera editor) draws what a camera sees into `art/views/<id>.png`; the newest 60 are kept. The prompt is `prefix, camera words, place, who is in view, suffix`: the camera words come from pitch and field of view (eye level, high or low angle, bird's eye, wide or telephoto lens), the place from the location's name and first sentence, and who is in view from projecting everyone in the scene through the camera (in the foreground, middle distance or distance, on the left, center or right). With an AUTOMATIC1111 server the live preview is sent as the `init_images` of `/sdapi/v1/img2img`, with the "Freedom" slider as the denoising strength (0.72 by default), so the composition follows the camera; an OpenAI-style server draws from the words alone. The picture size is 768x432. Style, model and negative prompt are the same as for the other pictures; `framing` and `prompts.json` do not apply to views.

### Modules with another look

The default style is gothic horror. For CyberHood (cyberpunk, 2040) put a replacement `prefix` in `art/style.json`, or per picture in `art/prompts.json` (for example `"locations/night_market": "neon cyberpunk night market, rain, holograms, cinematic"`), otherwise its locations are drawn in the gothic style.

### Gallery and git

The web client's Gallery panel lists every picture in this folder (characters, monsters, NPCs, locations, modules and camera views), newest first, from `GET /api/art/list`; click one to see it full size. The pictures themselves are not tracked: `art/.gitignore` ignores `*.png`, `*.jpg`, `*.jpeg`, `*.webp`, `*.part` and `views/`, while `style.json`, `prompts.json` and this file stay.

### Setting it up

`.\sd.ps1 -Check` (in the repository root) lists every requirement (git, Python 3.10, an NVIDIA GPU and driver, about 15 GB of disk, network access, AUTOMATIC1111, the model in the store, the port); `-Install` installs what is missing; `.\web.ps1` starts the image server with the game (`-NoArt` to skip it). The requirements and what each flag does are in the root [README](../../README.md#setting-up-stable-diffusion).

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
