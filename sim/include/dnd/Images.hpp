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
        {"items", "a single object, centered, game inventory icon, plain dark background, studio lighting"},
        {"locations", "wide establishing shot, eye level, no people"},
        {"modules", "sweeping panoramic establishing shot, low angle, no people"}};
};

// Mature mode for pictures: on with the same switch as the language model (--mature, DND_MATURE, content.mature in the
// agents file) or with "mature": {"enabled": true} in images.json. Everything here is optional and holds no words of its
// own: it is where the user puts the checkpoint and the words for their mature pictures. Empty or 0: the normal setting.
struct MatureImageStyle {
    std::string checkpoint;  // a model-store folder or a name the image server knows; usually a fine-tuned SD1.5 or SDXL build
    std::string prefix, suffix;  // replace the normal (and the module's) prefix and suffix
    std::string negative;        // added after the normal negative prompt
    std::map<std::string, std::string> framing;  // per kind, replaces the normal framing
    int steps = 0;
    double cfg = 0;
    std::string sampler;
    int portraitW = 0, portraitH = 0, wideW = 0, wideH = 0;  // SDXL wants about 1024 px
    bool narration = true;  // pictures drawn from a camera also say what the story last narrated
};

// Pictures of the story (camera views), images.json "story". A second language model, the illustrator (see Illustrator.hpp),
// keeps a condensed history of the story and writes each picture's prompt from it, and the picture is worded as a photograph.
// The illustrator's model: url and model empty use the game's own language model (whatever --llm-url / --ollama / agents
// serve); set them to give the illustrator its own. With no model at all, views are worded from the camera alone, as before.
struct StoryStyle {
    bool enabled = true;      // false: no illustrator, views are worded from the camera (and, in mature mode, the last narration)
    std::string url, path, model, apiKey, format;  // empty: the game's model; format "openai" (default) or "cppcoder"
    int timeoutSec = 120;
    double temperature = 0.4;
    int maxTokens = 700;
    int storyWords = 220, promptWords = 60;
    // A realistic photograph instead of the module's illustration: these words lead, the module's setting words follow with
    // their painting words taken out, and this negative replaces "photo" in the module's negative.
    bool photo = true;
    std::string prefix = "RAW photo, cinematic film still, photorealistic, 35mm photograph, natural skin texture, detailed realistic faces, natural lighting, film grain, sharp focus";
    std::string negative = "painting, drawing, illustration, digital art, concept art, cartoon, anime, comic, 3d render, cgi, video game, sketch, plastic skin, doll, airbrushed, oversaturated, deformed iris, bad anatomy, bad hands";
    std::string sampler = "DPM++ SDE Karras";  // Realistic Vision's own recommendation; empty: the normal sampler
    int steps = 30;
    double cfg = 5.5;
};

// The adults-only guard. In mature mode it is always applied, and nothing in any settings file can turn it off or
// override it: every picture of people says they are adults, this negative prompt comes after every other one, and a
// picture whose words mention a minor is refused rather than drawn.
const std::string& AdultsOnlyNegative();
bool MentionsMinor(const std::string& text);
// "adult woman", "adult man" or "adult" for a free-text gender ("female", "male", anything else or empty).
std::string AdultWords(const std::string& gender);

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
    bool mature = false;    // off by default; see MatureImageStyle
    MatureImageStyle matureStyle;
    StoryStyle story;
    const std::string& ActiveCheckpoint() const { return mature && !matureStyle.checkpoint.empty() ? matureStyle.checkpoint : checkpoint; }

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
    std::string module;   // the module whose art style (ModuleDef::art) words this picture; empty: the global style
    std::string gender;   // characters and NPCs: the free-text gender from the data, for the adults-only words
    bool person = false;  // shows a person (characters, NPCs, camera views)
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
    std::string initB64;      // base64 PNG to start from (img2img); empty: draw from nothing (txt2img)
    double denoise = 0.7;     // img2img only: 0 keeps the start picture, 1 ignores it
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

// A picture drawn from a camera: what the camera sees (already worded, see DescribeView), optionally a preview picture to
// start from so that the composition follows the camera. Written to <art dir>/views/<id>.png.
struct ViewJob {
    std::string id;           // letters, digits, - and _
    std::string subject;
    std::string initB64;      // base64 PNG of the preview, or empty
    double strength = 0.7;    // how far the picture may depart from the preview (img2img denoising strength)
    int width = 768, height = 432;
    std::uint32_t seed = 0;   // 0: derived from the id
    std::string module;       // the game's module, for its art style
    std::string narration;    // mature mode only: what the story last narrated, added to the subject
    // Runs on the picture worker before drawing (it may take a while: it asks the illustrator model). Returns the prompt
    // written from the story, or empty (err says why) to draw from subject and narration instead.
    std::function<std::string(std::string& err)> compose;
    std::string story;        // what compose wrote; replaces subject and narration
    std::string composeError; // why compose wrote nothing, or why its words were not used
    bool photo = false;       // word it as a photograph (StoryStyle)
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
    bool SetCheckpoint(const std::string& model, bool fromStore, std::string& err);  // in mature mode, sets the mature checkpoint
    bool Mature() const;
    // The words, size and settings a picture is drawn with. False (and err) when the adults-only guard refuses it.
    bool BuildRequest(const ArtSubject& s, const std::string& promptOverride, const ViewJob* view, ImageRequest& r, std::string& err) const;

    // Queue pictures. Request: {"missing": true, "kinds": [...]} or {"kind": "characters", "id": "thorin"} or
    // {"ids": ["characters/thorin", "monsters/zombie"]}; optional "force" (redo existing) and, for one id, "prompt".
    // Returns {queued, skipped} or sets err.
    Json Enqueue(const Json& req, std::string& err);
    // Queue a picture drawn from a camera. Returns {id, url} (the file appears at url when state is "done") or sets err.
    Json EnqueueView(const ViewJob& job, std::string& err);
    Json View(const std::string& id) const;  // {state: unknown|queued|running|done|failed, url, error}
    Json Jobs() const;  // {running, current, queued, done, failed, errors: [...], recent: [...]}
    void Cancel();      // drops everything still queued
    void WaitIdle();    // for tests and shutdown

    static std::string FindWeights(const std::string& dir);  // first .safetensors, .ckpt or .gguf under dir

private:
    struct Item {
        ArtSubject s;
        bool force = false;
        std::string promptOverride;
        std::shared_ptr<ViewJob> view;  // set for a picture drawn from a camera
    };
    void Worker();
    bool Do(const Item& it, std::string& err);
    std::string Existing(const std::string& kind, const std::string& id) const;
    std::string EffectiveCheckpoint(std::string& problem) const;
    bool MatureFallback() const;

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
    std::map<std::string, Json> views_;  // by id: {state, url, error}
    std::deque<std::string> viewOrder_;
    Json probe_;
    long long probeAt_ = 0;
    std::thread worker_;
};

}  // namespace dnd
