#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "dnd/Controller.hpp"
#include "dnd/Game.hpp"

namespace dnd {

// ---------------------------------------------------------------- model store

// A model living in the shared store (github.com/cschladetsch/CppLmmModelStore). The store only
// manages directories; weights are served by a separate OpenAI-compatible process (llama.cpp
// `llama-server`, Ollama, vLLM), which this app talks to over HTTP.
struct ModelInfo {
    std::string name;
    std::string home;    // the store root, e.g. ~/.local/share/deepseek/models
    std::string path;    // <home>/<name>
    bool exists = false;
    std::string weights; // first *.gguf found under path, if any
};
ModelInfo LocateModel(const std::string& name);

// ---------------------------------------------------------------- client

struct LlmConfig {
    std::string baseUrl = "http://127.0.0.1:8080";  // scheme://host:port (plain http; terminate TLS in a proxy)
    std::string path = "/v1/chat/completions";
    std::string model;                              // sent as "model"; also looked up in the model store
    std::string apiKey;
    int timeoutSec = 60;
    bool stream = true;
    double temperature = 0.7;
    int maxTokens = 400;

    // DND_LLM_URL, DND_LLM_PATH, DND_LLM_MODEL, DND_LLM_KEY, DND_LLM_TIMEOUT, DND_LLM_STREAM=0|1
    static LlmConfig FromEnv();
};

struct LlmReply {
    bool ok = false;
    std::string content;    // the visible answer (reasoning stripped)
    std::string reasoning;  // chain-of-thought from reasoning models, never shown to players
    std::string error;
};

class ILlmClient {
public:
    virtual ~ILlmClient() = default;
    virtual LlmReply Chat(const std::string& system, const std::string& user, bool jsonMode) = 0;
    virtual std::string Describe() const = 0;
};

// Removes <think>...</think> blocks that some servers inline instead of using reasoning_content.
std::string StripThink(std::string s);
// Extracts the first top-level {...} object from free text (models love code fences and chatter).
std::string ExtractJsonObject(const std::string& s);

class HttpLlmClient : public ILlmClient {
public:
    explicit HttpLlmClient(LlmConfig cfg) : cfg_(std::move(cfg)) {}
    LlmReply Chat(const std::string& system, const std::string& user, bool jsonMode) override;
    std::string Describe() const override { return cfg_.baseUrl + cfg_.path + " model=" + cfg_.model; }
    const LlmConfig& Config() const { return cfg_; }

private:
    LlmConfig cfg_;
};

// Scriptable client for tests and offline demos.
class MockLlmClient : public ILlmClient {
public:
    using Responder = std::function<LlmReply(const std::string& system, const std::string& user, bool jsonMode)>;
    explicit MockLlmClient(Responder r = nullptr) : responder_(std::move(r)) {}
    LlmReply Chat(const std::string& system, const std::string& user, bool jsonMode) override;
    std::string Describe() const override { return "mock"; }
    int Calls() const { return calls_; }
    std::string LastUser() const { return lastUser_; }

private:
    Responder responder_;
    int calls_ = 0;
    std::string lastUser_;
};

// ---------------------------------------------------------------- narration

// Prose for the events that benefit from it (scenes, downs, deaths, victories, quests). Failures
// degrade silently to the plain event text; after repeated failures the narrator backs off.
Game::Narrator MakeLlmNarrator(std::shared_ptr<ILlmClient> client, const Game& game);

// ---------------------------------------------------------------- LLM Dungeon Master

// Asks the model for a short plan (a few actions) at the start of a monster's turn, then replays it.
// Anything illegal, malformed or slow falls back to the built-in tactical AI for the rest of that turn,
// so the rules engine stays authoritative and a fight can never stall on the model.
class LlmDmController : public IController {
public:
    explicit LlmDmController(std::shared_ptr<ILlmClient> client);
    Action DecideAction(const TurnContext& ctx) override;
    std::unique_ptr<IController> Clone() const override { return std::make_unique<LlmDmController>(client_); }
    std::string_view Kind() const override { return "LLM"; }
    void OnActionResult(const TurnContext&, const Action&, bool ok, const std::string& reason) override;

    // Exposed for tests.
    static std::string BuildPrompt(const TurnContext& ctx);
    static std::vector<Action> ParsePlan(const std::string& text, std::string& err);

private:
    std::shared_ptr<ILlmClient> client_;
    std::unique_ptr<IController> fallback_;
    int planRound_ = -1, planActor_ = -1;
    std::vector<Action> plan_;
    bool useFallback_ = false;
};

}  // namespace dnd
