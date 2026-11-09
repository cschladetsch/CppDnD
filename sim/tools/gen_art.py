#!/usr/bin/env python3
"""Generate pictures for CppDnD from its JSON data with a local Stable Diffusion server.

Talks to the AUTOMATIC1111 web API (also served by Forge and SD.Next): POST /sdapi/v1/txt2img.
Writes art/<kind>/<id>.png, which dnd_server serves and the web client picks up automatically.
Each picture uses a seed derived from its id, so regenerating gives the same picture until the
prompt or style changes. Existing files are skipped unless --force.

  python tools/gen_art.py --check                      is the server reachable?
  python tools/gen_art.py --list                       show every prompt, generate nothing
  python tools/gen_art.py                              generate whatever is missing
  python tools/gen_art.py --only characters --force    redo one kind
  python tools/gen_art.py --ids thorin,zombie          just these ids

To replace a generated subject, put "kind/id": "text" in art/prompts.json (the server reads the same file), or
give a data entry {"art": {"prompt": "..."}}.
"""
import argparse, base64, binascii, json, os, re, sys, urllib.error, urllib.request, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.dirname(HERE)
FRAMING = {
    "characters": "upper body portrait, looking at the viewer, eye level, shallow depth of field",
    "npcs": "upper body portrait, looking at the viewer, eye level, shallow depth of field",
    "monsters": "full body, three-quarter view, centered, plain dark background",
    "locations": "wide establishing shot, eye level, no people",
    "modules": "sweeping panoramic establishing shot, low angle, no people",
}
CREATURE_TYPES = {"humanoid", "undead", "beast", "giant", "fiend", "celestial", "construct", "dragon", "elemental", "fey", "monstrosity", "ooze", "plant", "aberration"}


def load_json(path):
    text = open(path, encoding="utf-8").read()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"^\s*//.*$", "", text, flags=re.M)
    return json.loads(text)


def article(word):
    return "an" if word[:1].lower() in "aeiou" and word else "a"


def override(entry, subject):
    art = entry.get("art") if isinstance(entry, dict) else None
    return art["prompt"] if isinstance(art, dict) and art.get("prompt") else subject


def jobs(data_dir):
    """Yield (kind, id, subject, shape) for everything that can have a picture."""
    for c in load_json(os.path.join(data_dir, "characters.json")):
        race = [t for t in c.get("tags", []) if t not in CREATURE_TYPES] or ["human"]
        cls = c.get("class", "adventurer")
        yield "characters", c["id"], override(c, f"{c.get('name', c['id'])}, {article(' '.join(race))} {' '.join(race)} {cls}, adventurer"), "portrait"
    for m in load_json(os.path.join(data_dir, "monsters.json")):
        kinds = [t for t in m.get("tags", [])]
        yield "monsters", m["id"], override(m, f"{m.get('name', m['id']).lower()}, {' '.join(kinds)} monster, dungeons and dragons creature, menacing"), "portrait"
    modules_dir = os.path.join(data_dir, "modules")
    for fn in sorted(os.listdir(modules_dir)) if os.path.isdir(modules_dir) else []:
        if not fn.endswith(".json"):
            continue
        mod = load_json(os.path.join(modules_dir, fn))
        yield "modules", mod["id"], override(mod, f"{mod.get('name', mod['id'])}, {mod.get('setting', '')}"), "wide"
        for loc in mod.get("locations", []):
            yield "locations", loc["id"], override(loc, f"{loc.get('name', loc['id'])}, {loc.get('kind', '')}, {loc.get('description', '')}"), "wide"
            for n in loc.get("npcs", []):
                yield "npcs", n["id"], override(n, f"{n.get('name', n['id'])}, {article(n.get('type', 'commoner'))} {n.get('type', 'commoner')}. {n.get('description', '')}"), "portrait"


def post(url, payload, timeout):
    req = urllib.request.Request(url, json.dumps(payload).encode(), {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", default=os.environ.get("SD_URL", "http://127.0.0.1:7860"), help="Stable Diffusion server (default %(default)s, or $SD_URL)")
    ap.add_argument("--data", default=os.path.join(SIM, "data"))
    ap.add_argument("--out", default=os.path.join(SIM, "art"))
    ap.add_argument("--style", default=None, help="style file (default <out>/style.json)")
    ap.add_argument("--only", default="", help="comma list of: characters, monsters, npcs, locations, modules")
    ap.add_argument("--ids", default="", help="comma list of ids")
    ap.add_argument("--force", action="store_true", help="regenerate pictures that already exist")
    ap.add_argument("--list", action="store_true", help="print the prompts and stop")
    ap.add_argument("--check", action="store_true", help="only test that the server answers")
    ap.add_argument("--timeout", type=int, default=600, help="seconds to wait per picture")
    a = ap.parse_args()

    if a.check:
        try:
            with urllib.request.urlopen(a.url.rstrip("/") + "/sdapi/v1/options", timeout=5):
                print(f"ok: {a.url} answers")
                return 0
        except Exception as e:
            print(f"cannot reach {a.url}: {e}\nStart AUTOMATIC1111 / Forge with --api (and --listen if remote).", file=sys.stderr)
            return 1

    style_path = a.style or os.path.join(a.out, "style.json")
    style = load_json(style_path) if os.path.exists(style_path) else {}
    steps, cfg = style.get("steps", 25), style.get("cfg", 6.5)
    sizes = {"portrait": style.get("portrait", [512, 512]), "wide": style.get("wide", [768, 432])}
    prompts_path = os.path.join(a.out, "prompts.json")
    prompts = load_json(prompts_path) if os.path.exists(prompts_path) else {}
    only = {s for s in a.only.split(",") if s}
    ids = {s for s in a.ids.split(",") if s}

    todo = [j for j in jobs(a.data) if (not only or j[0] in only) and (not ids or j[1] in ids)]
    if not todo:
        print("nothing matches", file=sys.stderr)
        return 1
    made = skipped = failed = 0
    for kind, ident, subject, shape in todo:
        custom = isinstance(prompts.get(f"{kind}/{ident}"), str)
        subject = prompts[f"{kind}/{ident}"] if custom else subject
        framing = "" if custom else {**FRAMING, **style.get("framing", {})}.get(kind, "")
        path = os.path.join(a.out, kind, ident + ".png")
        prompt = ""
        for part in (style.get("prefix", ""), subject, framing, style.get("suffix", "")):
            if part:
                prompt += ("" if not prompt else " " if prompt[-1] in ",." else ", ") + part
        if a.list:
            print(f"{kind}/{ident}.png  {sizes[shape][0]}x{sizes[shape][1]}  seed {zlib.crc32(ident.encode()) & 0x7fffffff}\n    {prompt}")
            continue
        if os.path.exists(path) and not a.force:
            skipped += 1
            continue
        payload = {"prompt": prompt, "negative_prompt": style.get("negative", ""), "seed": zlib.crc32(ident.encode()) & 0x7FFFFFFF,
                   "steps": steps, "cfg_scale": cfg, "width": sizes[shape][0], "height": sizes[shape][1], "sampler_name": style.get("sampler", "DPM++ 2M Karras")}
        if style.get("checkpoint"):
            payload["override_settings"] = {"sd_model_checkpoint": style["checkpoint"]}
        print(f"{kind}/{ident} ...", end=" ", flush=True)
        try:
            img = post(a.url.rstrip("/") + "/sdapi/v1/txt2img", payload, a.timeout)["images"][0]
            raw = base64.b64decode(img.split(",", 1)[-1])
            os.makedirs(os.path.dirname(path), exist_ok=True)
            tmp = path + ".part"
            with open(tmp, "wb") as f:
                f.write(raw)
            os.replace(tmp, path)
            made += 1
            print("done")
        except (urllib.error.URLError, OSError, KeyError, IndexError, ValueError, binascii.Error) as e:
            failed += 1
            print(f"failed: {e}")
            if isinstance(e, urllib.error.URLError) and not isinstance(e, urllib.error.HTTPError):
                print("server unreachable; stopping", file=sys.stderr)
                break
    if not a.list:
        print(f"{made} made, {skipped} already there, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
