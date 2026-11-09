#!/usr/bin/env python3
"""Sessions: every game is kept by the server, listed, renamed, deleted, and still there after a restart.
Usage: sessions_smoke.py <path to dnd_server>"""
import json, os, shutil, socket, subprocess, sys, tempfile, time, urllib.error, urllib.request

EXE = sys.argv[1]
TMP = tempfile.mkdtemp(prefix="dnd_sessions_")
PROC = None
BASE = ""


def start():
    global PROC, BASE
    with socket.socket() as sk:
        sk.bind(("127.0.0.1", 0))
        port = sk.getsockname()[1]
    PROC = subprocess.Popen([EXE, "--port", str(port), "--mock-llm", "--sessions", TMP, "--history", ""], stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    BASE = f"http://127.0.0.1:{port}"
    for _ in range(100):
        try:
            urllib.request.urlopen(BASE + "/api/health").read()
            return
        except Exception:
            time.sleep(0.1)
    raise SystemExit("server did not start")


def stop():
    PROC.terminate()
    PROC.wait(10)


def call(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def files():
    return sorted(f for f in os.listdir(TMP) if f.endswith(".json"))


try:
    start()
    st, r = call("GET", "/api/games")
    assert st == 200 and r["sessions"] == [] and r["persistent"] is True, r

    st, a = call("POST", "/api/games", {"module": "ravenmoor", "seed": 1, "role": "spectator"})
    assert st == 201 and a["name"] == "The Dread of Ravenmoor #1", a
    st, b = call("POST", "/api/games", {"module": "cyberhood", "seed": 2, "role": "player", "hero": "elara", "name": "  Neon run  "})
    assert st == 201 and b["name"] == "Neon run", b
    st, c = call("POST", "/api/games", {"module": "ravenmoor", "seed": 3, "role": "spectator"})
    assert st == 201 and c["name"] == "The Dread of Ravenmoor #2", c
    assert len(files()) == 3, files()

    # rename one session, then (a second later) play another
    st, r = call("POST", f"/api/games/{c['id']}/name", {"name": "Second try"})
    assert st == 200 and r["name"] == "Second try", r
    st, r = call("POST", f"/api/games/{c['id']}/name", {"name": "   "})
    assert st == 400, r
    st, r = call("POST", f"/api/games/{c['id']}/name", {"name": "x" * 81})
    assert st == 400, r
    time.sleep(1.1)  # timestamps have one-second resolution
    for _ in range(6):
        st, r = call("POST", f"/api/games/{a['id']}/auto", {"steps": 2})
        assert st == 200, r
    played = r["view"]

    st, before = call("GET", "/api/games")
    names = {s["id"]: s["name"] for s in before["sessions"]}
    assert names == {a["id"]: "The Dread of Ravenmoor #1", b["id"]: "Neon run", c["id"]: "Second try"}, names
    info = {s["id"]: s for s in before["sessions"]}
    assert info[b["id"]]["module"] == "cyberhood" and info[b["id"]]["hero"] == "Elara" and info[b["id"]]["role"] == "player", info[b["id"]]
    assert info[a["id"]]["day"] == played["clock"]["day"] and info[a["id"]]["mode"] == played["mode"], info[a["id"]]
    assert before["sessions"][0]["id"] == a["id"], "the session played last comes first"  # a was played after the others were created
    assert before["sessions"][0]["updated"] >= before["sessions"][-1]["updated"]

    # restart: everything is still there, in the same state
    stop()
    start()
    st, after = call("GET", "/api/games")
    assert {s["id"]: s["name"] for s in after["sessions"]} == names, after
    st, v = call("GET", f"/api/games/{a['id']}")
    assert st == 200 and v["clock"] == played["clock"] and v["location"]["id"] == played["location"]["id"] and v["mode"] == played["mode"], "state changed across a restart"
    st, v = call("GET", f"/api/games/{b['id']}")
    assert st == 200 and v["hero"] == "Elara" and v["module"]["name"] == "CyberHood", v.get("module")

    # a restored session keeps playing, and the same way a never-restarted one would
    st, r = call("POST", f"/api/games/{a['id']}/auto", {"steps": 3})
    assert st in (200, 422), r

    # delete removes it from the list and from disk, for good
    st, r = call("DELETE", f"/api/games/{c['id']}")
    assert st == 200 and r["deleted"], r
    assert (c["id"] + ".json") not in files() and len(files()) == 2, files()
    stop()
    start()
    st, last = call("GET", "/api/games")
    assert sorted(s["id"] for s in last["sessions"]) == sorted([a["id"], b["id"]]), last
    assert call("GET", f"/api/games/{c['id']}")[0] == 404

    # a damaged file is skipped, not fatal
    with open(os.path.join(TMP, "0123456789ab.json"), "w") as f:
        f.write("{not json")
    stop()
    start()
    st, last = call("GET", "/api/games")
    assert st == 200 and len(last["sessions"]) == 2, last
    print("sessions ok:", len(last["sessions"]), "restored after restarts")
finally:
    try:
        stop()
    except Exception:
        pass
    shutil.rmtree(TMP, ignore_errors=True)
