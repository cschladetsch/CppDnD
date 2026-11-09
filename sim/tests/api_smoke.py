#!/usr/bin/env python3
"""Plays a whole module through the HTTP API: the human hero acts via /action, everything else via
/advance and /auto. Usage: api_smoke.py http://127.0.0.1:8088 [seed]"""
import json, os, socket, subprocess, sys, time, urllib.request, urllib.error

BASE = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8088"
SEED = int(sys.argv[2]) if len(sys.argv) > 2 else 3
SERVER = None
if BASE.startswith("spawn:"):  # spawn:<path to dnd_server>: start it on a free port for the duration of the test
    with socket.socket() as sk:
        sk.bind(("127.0.0.1", 0))
        port = sk.getsockname()[1]
    SERVER = subprocess.Popen([BASE[6:], "--port", str(port), "--mock-llm", "--sessions", ""], stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    BASE = f"http://127.0.0.1:{port}"
    for _ in range(100):
        try:
            urllib.request.urlopen(BASE + "/api/health").read()
            break
        except Exception:
            time.sleep(0.1)


def call(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req) as r:
            return r.status, json.load(r)
    except urllib.error.HTTPError as e:
        return e.code, json.load(e)


def dist(a, b):
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]))


def step_toward(view, me, tgt):
    rows = view["combat"]["grid"]["rows"]
    occupied = {tuple(a["pos"]) for a in view["combat"]["actors"] if a["life"] != "dead"}
    best, bd = None, 10**9
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            p = (me["pos"][0] + dx, me["pos"][1] + dy)
            if (dx or dy) and 0 <= p[1] < len(rows) and 0 <= p[0] < len(rows[0]) and rows[p[1]][p[0]] in ".~" and p not in occupied:
                d = dist(p, tgt["pos"])
                if d < bd:
                    best, bd = p, d
    return best


def human_action(view):
    c = view["combat"]
    me = next(a for a in c["actors"] if a["id"] == c["awaiting"])
    foes = [a for a in c["actors"] if a["team"] == "monsters" and a["life"] != "dead"]
    standing = [a for a in foes if a["life"] == "alive"] or foes
    tgt = min(standing, key=lambda a: dist(a["pos"], me["pos"]))
    opts = c["options"]
    if opts["economy"]["action"] or opts["economy"]["attacksLeft"]:
        for sp in opts["spells"]:
            if sp["id"] == "fire_bolt" and dist(tgt["pos"], me["pos"]) * 5 <= sp["rangeFt"]:
                return {"type": "cast", "spell": "fire_bolt", "target": tgt["id"]}
        if dist(tgt["pos"], me["pos"]) <= 1 and opts["attacks"]:
            return {"type": "attack", "target": tgt["id"], "attack": 0}
    if opts["economy"]["movementFt"] >= 5:
        p = step_toward(view, me, tgt)
        if p and dist(p, tgt["pos"]) < dist(me["pos"], tgt["pos"]):
            return {"type": "move", "dest": list(p)}
    return {"type": "end_turn"}


st, r = call("POST", "/api/games", {"module": "ravenmoor", "seed": SEED, "role": "player", "hero": "elara"})
assert st == 201, r
gid, view = r["id"], r["view"]
print("game", gid, view["module"]["name"])
fails = 0
for step in range(4000):
    mode = view["mode"]
    if mode in ("complete", "defeated"):
        break
    if mode == "combat":
        c = view["combat"]
        if c["awaiting"] >= 0:
            st, r = call("POST", f"/api/games/{gid}/action", human_action(view))
            if st == 422:
                fails += 1
                st, r = call("POST", f"/api/games/{gid}/action", {"type": "end_turn"})
        else:
            st, r = call("POST", f"/api/games/{gid}/advance")
        assert st in (200, 422), (st, r)
        view = r["view"]
    else:
        st, r = call("POST", f"/api/games/{gid}/auto", {"steps": 1})
        if st == 422:
            print("auto stopped:", r["message"])
            break
        view = r["view"]

print("final mode:", view["mode"], "| day", view["clock"]["day"], "| quests", sum(q["done"] for q in view["quests"]), "/", len(view["quests"]),
      "| rejected human actions:", fails)
st, save = call("GET", f"/api/games/{gid}/save")
assert st == 200 and save["version"] == 1
st, loaded = call("POST", "/api/games/load", save)
assert st == 201 and loaded["view"]["mode"] == view["mode"], "save/load round trip failed"
print("save/load ok; new id", loaded["id"])
if SERVER:
    SERVER.terminate()
assert view["mode"] in ("complete", "defeated"), "game did not finish"
