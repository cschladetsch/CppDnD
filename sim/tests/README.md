# Tests

| File | Covers |
|---|---|
| `tests.cpp` | `dnd_tests`: dice and RNG, grid and cover, rules (advantage, concentration, damage adjustment, Extra Attack), content loading, JSON round trips, save and load determinism, modules, rests, role swap, the LLM pipeline with a mock model, and the plain-English interpreter |
| `api_smoke.py` | plays a whole module through the HTTP API: creating a game, acting as the hero, autopilot, saving and loading |

Both are registered with CTest.

```powershell
.\test.ps1              # build, then run everything with failure output
.\test.ps1 -Rebuild     # clean configure first
ctest --test-dir sim/build --output-on-failure -R dnd_tests     # one suite
```

Run the smoke test against a server you started yourself:

```powershell
python sim/tests/api_smoke.py http://127.0.0.1:8088 7
```

or let it start one on a free port with `spawn:<path to dnd_server>`, which is what CTest does (the server runs with `--mock-llm`).

## Adding a test

Write a `static void TestX()` in `tests.cpp` using `CHECK(...)`, and add it to the table in `main`. Each test runs inside its own try/catch, so an exception is reported by name instead of aborting the run. Tests that need a model use `MockLlmClient` with a responder lambda; no network is involved.
