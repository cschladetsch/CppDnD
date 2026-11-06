# sim

The simulator: a rules engine, a terminal client, an HTTP server with a browser client, content, and tests. Features, build and run instructions are in the [root README](../README.md).

## How it fits together

```
                 data/*.json  (content, loaded once at startup)
                      |
  main.cpp ---> Game (exploration, clock, rests, quests) ---> Combat (grid, initiative, rules)
  server_main.cpp --+        |                                   |
  (HTTP /api + web/)         +-- Llm (narration, DM, English) +--- controllers: AI, Human, Remote, LlmDm
```

- `Game` is a session: a party moving through a module on a clock. It starts a `Combat` when a fight breaks out and returns to exploring when it ends. Everything is step-driven, so one server can drive many games.
- `Combat` runs initiative and the rules on a `TacticalGrid`. Each creature is steered by an `IController`: the built-in AI, a terminal human, a remote human (the web client), or a model-run Dungeon Master. Swapping controllers at runtime is how role swap works.
- `Json` converts everything to and from JSON for saves and the API. Each game owns its own seeded RNG, so saves replay exactly.
- `Llm` talks to any OpenAI-compatible server. It is only ever asked to propose; the rules engine validates and falls back to the AI.

## Targets

| Target | What |
|---|---|
| `dnd_core` | the engine library (everything except the two `main` files) |
| `dnd_sim` | terminal client |
| `dnd_server` | HTTP server; also serves `web/` at `/` |
| `dnd_tests` | unit and integration tests; CTest also runs `tests/api_smoke.py` against a spawned server |

## Folders

| Folder | Contents |
|---|---|
| [data](data/README.md) | game content and how to author it |
| [include](include/README.md) | public headers, one per subsystem |
| [src](src/README.md) | implementation, one file per header plus the two entry points |
| [web](web/README.md) | browser client |
| [tests](tests/README.md) | test suites |
| [docs](docs/README.md) | HTTP API reference |
| [third_party](third_party/README.md) | vendored libraries |
