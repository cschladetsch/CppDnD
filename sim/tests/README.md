# Tests

| File | Covers |
|---|---|
| `main.cpp` | the GoogleTest entry point; loads the game content once for every test |
| `test_support.hpp` | shared helpers (`Hero`, `Mon`) |
| `test_core.cpp` | dice, grid and cover, rules (advantage, concentration, damage adjustment, Extra Attack), content loading, RNG, JSON round trips |
| `test_game.cpp` | remote combat, modules, save and load determinism, procedural delves, role swap, switching the controlled hero mid-fight |
| `test_llm.cpp` | the LLM pipeline with a mock model, spoken lines, per-actor agents |
| `test_history.cpp` | the pair and group histories (files, saves, fights, NPC talk, private agent lines) |
| `test_images.cpp` | picture subjects, configuration files, the art service queue with a mock image client, model store resolution |
| `test_art.cpp` | the AUTOMATIC1111 client against a fake local server (txt2img, img2img, errors, probe) and camera pictures through the art service (files, seeds, failures, bad ids) |
| `test_scene.cpp` | one end-to-end pass over the camera, scene and prompt words |
| `test_camera.cpp` | the camera model in detail: clamping, projection for every heading, lens and pitch, camera words, scene layout, the words a view produces (parameterized) |
| `api_smoke.py` | plays a whole module through the HTTP API: creating a game, acting as the hero, autopilot, saving and loading |

Every `TEST` is its own CTest entry (`gtest_discover_tests`), so `ctest` reports them one by one; `api_smoke` is registered as well. GoogleTest is taken from the system if installed, otherwise fetched (v1.15.2) at configure time.


```powershell
.\test.ps1              # build, then run everything with failure output
.\test.ps1 -Rebuild     # clean configure first
ctest --test-dir sim/build --output-on-failure -R Camera          # tests whose name matches
sim/build/dnd_tests --gtest_filter=DescribeView.*                # straight from the binary
sim/build/dnd_tests --gtest_list_tests                           # what exists
```

Run the smoke test against a server you started yourself:

```powershell
python sim/tests/api_smoke.py http://127.0.0.1:8088 7
```

or let it start one on a free port with `spawn:<path to dnd_server>`, which is what CTest does (the server runs with `--mock-llm`).

## Adding a test

Write a `TEST(Suite, Name)` (or `TEST_P` for a table of cases) in the file that fits, using `EXPECT_*` for checks that should not stop the test and `ASSERT_*` for ones it cannot continue without. A new `.cpp` goes in the `dnd_tests` source list in `sim/CMakeLists.txt`. Tests that need a model use `MockLlmClient`; ones that need an image server use `MockImageClient` or the `FakeA1111` in `test_art.cpp`. No test touches the real network.
