# third_party

Vendored, header-only, MIT licensed.

| Library | Version | Used for |
|---|---|---|
| `nlohmann/json.hpp` | 3.11.3 | all JSON: content files, saves, the API |
| `httplib.h` (cpp-httplib) | 0.15.3 | the HTTP server, and the HTTP client for LLM calls |

To upgrade, replace the file with the release you want, rebuild, and run `.\test.ps1`.

CppLmmModelStore is not vendored: CMake fetches it at configure time (see `DND_USE_MODELSTORE` and `DND_MODELSTORE_DIR` in the root README).
