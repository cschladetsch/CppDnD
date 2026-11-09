#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "dnd/Json.hpp"

namespace dnd {

// ---------------------------------------------------------------- configuration

// How pictures are worded: the words around each subject, and the size and sampling settings.
struct ImageStyle {
    std::string prefix = "dark fantasy gothic horror illustration, painterly, moody dramatic lighting, rich detail,";
    std::string suffix;
    std::string negative = "text, watermark, signature, logo, lowres, blurry, jpeg artifacts, deformed, extra limbs, extra fingers, modern clothing, photo";
    int steps = 25;
    double cfg = 6.5;
    std::string sampler = "DPM++ 2M Karras";
    int portraitW = 512, portraitH = 512;  // characters, monsters, NPCs
    int wideW = 768, wideH = 432;          // locations, modules
    // How each kind of picture is framed: shot size, angle, composition. Appended after the subject. (A text prompt is
    // the only camera a Stable Diffusion model has; these words and the picture's shape are all there is to steer it.)
    std::map<std::string, std::string> framing = {
        {"characters", "upper body portrait, looking at the viewer, eye level, shallow depth of field"},
        {"npcs", "upper body portrait, looking at the viewer, eye level, shallow depth of field"},
        {"monsters", "full body, three-quarter view, centered, plain dark background"},
        {"locations", "wide establishing shot, eye level, no people"},
        {"modules", "sweeping panoramic establishing shot, low angle, no people"}};
};

// Where pictures come from. Two kinds of server are understood:
//   "a1111"  AUTOMATIC1111, Forge, SD.Next (started with --api): POST /sdapi/v1/txt2img
//   "openai" anything serving POST /v1/images/generations with b64_json (LocalAI, and others)
// The model is a name in the CppLmmModelStore (a folder holding a .safetensors or .ckpt), or a name the server knows.
struct ImageConfig {
    bool enabled = true;
    std::string backend = "a1111";
    std::string url = "http://127.0.0.1:7860";
    std::string path;           // openai only; default /v1/images/generations
    std::string apiKey;
    std::string checkpoint;     // empty: whatever the server has loaded
    bool requireStore = false;  // the model must exist in the model store
    int timeoutSec = 600;
    ImageStyle style;
    std::map<std::string, std::string> prompts;  // "characters/thorin" -> replacement for the generated subject

    // images.json (data/images.json, --images, DND_IMAGES), then the style file (art/style.json), then art/prompts.json.
    // Any of them may be missing; a malformed one is an error.
    static std::optional<ImageConfig> Load(const std::string& imagesFile, const std::string& styleFile, const std::string& promptsFile, std::string& err);
};

// ---------------------------------------------------------------- what can be drawn

struct ArtSubject {
    std::string kind;     // characters | monsters | npcs | locations | modules
    std::string id;
    std::string subject;  // what the picture shows, built from the JSON content (or prompts.json)
    bool wide = false;
    bool custom = false;  // the subject came from prompts.json and is used as written, with no framing added
};
// Everything in the loaded content that can have a picture.
std::vector<ArtSubject> ArtSubjects(const std::map<std::string, std::string>& overrides = {});
std::uint32_t Crc32(const std::string& s);  // a stable seed per id

// ---------------------------------------------------------------- clients

struct ImageRequest {
    std::string prompt, negative, sampler, checkpoint;
    std::uint32_t seed = 0;
    int steps = 25, width = 512, height = 512;
    double cfg = 6.5;
};
struct ImageResult {
    bool ok = false;
    std::string error;
    std::string bytes;  // PNG or JPEG
    bool unreachable = false;  // the server could not be reached at all (as opposed to refusing one picture)
};
class IImageClient {
public:
    virtual ~IImageClient() = default;
    virtual ImageResult Generate(const ImageRequest& r) = 0;
    // {"reachable": bool, "error": "...", "models": [names the server has]}
    virtual Json Probe() = 0;
    virtual std::string Describe() const = 0;
};
std::shared_ptr<IImageClient> MakeImageClient(const ImageConfig& cfg);

class MockImageClient : public IImageClient {
public:
    using Fn = std::function<ImageResult(const ImageRequest&)>;
    explicit MockImageClient(Fn f) : fn_(std::move(f)) {}
    ImageResult Generate(const ImageRequest& r) override { return fn_(r); }
    Json Probe() override { return Json{{"reachable", true}, {"models", Json::array()}}; }
    std::string Describe() const override { return "mock image client"; }

private:
    Fn fn_;
};

// ---------------------------------------------------------------- the art service

// Draws pictures in the background, one at a time, into <art dir>/<kind>/<id>.png (or .jpg), where the server
// already serves them and the web client already looks. The model is chosen through the CppLmmModelStore:
// a name that exists there is resolved to its weights file; the image server must be able to load that file
// (for AUTOMATIC1111 or Forge: start it with --ckpt-dir pointing at the store).
class ArtService {
public:
    ArtService(ImageConfig cfg, std::string artDir, std::shared_ptr<IImageClient> client = nullptr);
    ~ArtService();
    ArtService(const ArtService&) = delete;
    ArtService& operator=(const ArtService&) = delete;

    // {enabled, backend, url, reachable, error, checkpoint, effectiveCheckpoint, store: {...}, models: {store: [...], server: [...]}, problem}
    Json Status();
    bool SetCheckpoint(const std::string& model, bool fromStore, std::string& err);

    // Queue pictures. Request: {"missing": true, "kinds": [...]} or {"kind": "characters", "id": "thorin"} or
    // {"ids": ["characters/thorin", "monsters/zombie"]}; optional "force" (redo existing) and, for one id, "prompt".
    // Returns {queued, skipped} or sets err.
    Json Enqueue(const Json& req, std::string& err);
    Json Jobs() const;  // {running, current, queued, done, failed, errors: [...], recent: [...]}
    void Cancel();      // drops everything still queued
    void WaitIdle();    // for tests and shutdown

    static std::string FindWeights(const std::string& dir);  // first .safetensors, .ckpt or .gguf under dir

private:
    struct Item {
        ArtSubject s;
        bool force = false;
        std::string promptOverride;
    };
    void Worker();
    bool Do(const Item& it, std::string& err);
    std::string Existing(const std::string& kind, const std::string& id) const;
    std::string EffectiveCheckpoint(std::string& problem) const;

    ImageConfig cfg_;
    std::string artDir_;
    std::shared_ptr<IImageClient> client_;
    mutable std::mutex m_;
    std::condition_variable cv_, idle_;
    std::deque<Item> queue_;
    std::string current_;
    bool busy_ = false, stop_ = false;
    int done_ = 0, failed_ = 0;
    std::vector<std::string> errors_, recent_;
    Json probe_;
    long long probeAt_ = 0;
    std::thread worker_;
};

}  // namespace dnd
