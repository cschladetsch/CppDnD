#include "dnd/Images.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>

#include "dnd/Content.hpp"
#include "dnd/Llm.hpp"
#include "dnd/Util.hpp"
#include "httplib.h"

namespace dnd {
namespace fs = std::filesystem;
namespace {

std::string LowerCase(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Base64Decode(const std::string& in) {
    static const auto table = [] {
        std::array<int, 256> t;
        t.fill(-1);
        const std::string a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (std::size_t i = 0; i < a.size(); ++i) t[static_cast<unsigned char>(a[i])] = static_cast<int>(i);
        t[static_cast<unsigned char>('-')] = 62;  // url-safe alphabet too
        t[static_cast<unsigned char>('_')] = 63;
        return t;
    }();
    std::string out;
    out.reserve(in.size() * 3 / 4);
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        const int d = table[c];
        if (d < 0) continue;  // padding, whitespace
        val = (val << 6) + d;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

// "png" | "jpg" | "webp" | "" by the file's first bytes.
std::string Sniff(const std::string& b) {
    if (b.size() > 8 && b.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0) return "png";
    if (b.size() > 3 && static_cast<unsigned char>(b[0]) == 0xFF && static_cast<unsigned char>(b[1]) == 0xD8) return "jpg";
    if (b.size() > 12 && b.compare(0, 4, "RIFF") == 0 && b.compare(8, 4, "WEBP") == 0) return "webp";
    return "";
}

const char* const kExts[] = {"png", "jpg", "jpeg", "webp", "svg"};

bool ReadJsonFile(const std::string& path, Json& out, std::string& err) {
    if (path.empty()) return false;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot read " + path; return false; }
    out = Json::parse(in, nullptr, false, true);  // comments allowed
    if (out.is_discarded()) { err = path + " is not valid JSON"; return false; }
    return true;
}

std::string Article(const std::string& word) { return !word.empty() && std::string("aeiouAEIOU").find(word[0]) != std::string::npos ? "an" : "a"; }

std::string Join(const std::vector<std::string>& v, const char* sep = " ") {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : sep) + x;
    return s;
}

long long NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---------------------------------------------------------------- backends

struct Endpoint {
    std::string url;
};

class A1111Client : public IImageClient {
public:
    explicit A1111Client(ImageConfig c) : cfg_(std::move(c)) {}
    ImageResult Generate(const ImageRequest& r) override {
        ImageResult out;
        Json body{{"prompt", r.prompt}, {"negative_prompt", r.negative}, {"seed", r.seed}, {"steps", r.steps}, {"cfg_scale", r.cfg},
                  {"width", r.width}, {"height", r.height}, {"sampler_name", r.sampler}, {"batch_size", 1}, {"n_iter", 1}};
        if (!r.checkpoint.empty()) body["override_settings"] = Json{{"sd_model_checkpoint", r.checkpoint}};
        try {
            httplib::Client cli(cfg_.url);
            cli.set_connection_timeout(5);
            cli.set_read_timeout(cfg_.timeoutSec);
            cli.set_write_timeout(10);
            httplib::Headers h;
            if (!cfg_.apiKey.empty()) h.emplace("Authorization", "Bearer " + cfg_.apiKey);
            auto res = cli.Post("/sdapi/v1/txt2img", h, body.dump(), "application/json");
            if (!res) {
                out.error = "image server request failed: " + httplib::to_string(res.error());
                out.unreachable = res.error() == httplib::Error::Connection || res.error() == httplib::Error::ConnectionTimeout;
                return out;
            }
            if (res->status < 200 || res->status >= 300) { out.error = Cat("image server HTTP ", res->status, ": ", res->body.substr(0, 200)); return out; }
            const Json j = Json::parse(res->body, nullptr, false);
            if (j.is_discarded() || !j.contains("images") || !j["images"].is_array() || j["images"].empty() || !j["images"][0].is_string()) { out.error = "image server sent no image"; return out; }
            std::string b64 = j["images"][0].get<std::string>();
            if (const auto comma = b64.find(','); comma != std::string::npos && b64.compare(0, 5, "data:") == 0) b64.erase(0, comma + 1);
            out.bytes = Base64Decode(b64);
            out.ok = !out.bytes.empty();
            if (!out.ok) out.error = "image server sent an empty image";
        } catch (const std::exception& e) { out.error = e.what(); }
        return out;
    }
    Json Probe() override {
        Json p{{"reachable", false}, {"models", Json::array()}};
        try {
            httplib::Client cli(cfg_.url);
            cli.set_connection_timeout(2);
            cli.set_read_timeout(5);
            httplib::Headers h;
            if (!cfg_.apiKey.empty()) h.emplace("Authorization", "Bearer " + cfg_.apiKey);
            auto res = cli.Get("/sdapi/v1/sd-models", h);
            if (!res) { p["error"] = "cannot reach " + cfg_.url + " (" + httplib::to_string(res.error()) + ")"; return p; }
            if (res->status != 200) { p["error"] = Cat("HTTP ", res->status, " from ", cfg_.url, "; is it started with --api?"); return p; }
            p["reachable"] = true;
            const Json j = Json::parse(res->body, nullptr, false);
            if (j.is_array())
                for (const auto& m : j) {
                    const std::string name = m.value("filename", "");
                    p["models"].push_back(name.empty() ? m.value("model_name", m.value("title", "")) : fs::path(name).filename().string());
                }
        } catch (const std::exception& e) { p["error"] = e.what(); }
        return p;
    }
    std::string Describe() const override { return "a1111 at " + cfg_.url; }

private:
    ImageConfig cfg_;
};

class OpenAiImageClient : public IImageClient {
public:
    explicit OpenAiImageClient(ImageConfig c) : cfg_(std::move(c)) {
        if (cfg_.path.empty()) cfg_.path = "/v1/images/generations";
    }
    ImageResult Generate(const ImageRequest& r) override {
        ImageResult out;
        Json body{{"prompt", r.prompt}, {"n", 1}, {"size", Cat(r.width, "x", r.height)}, {"response_format", "b64_json"},
                  {"negative_prompt", r.negative}, {"seed", r.seed}, {"steps", r.steps}, {"cfg_scale", r.cfg}};
        if (!r.checkpoint.empty()) body["model"] = r.checkpoint;
        try {
            httplib::Client cli(cfg_.url);
            cli.set_connection_timeout(5);
            cli.set_read_timeout(cfg_.timeoutSec);
            cli.set_write_timeout(10);
            httplib::Headers h;
            if (!cfg_.apiKey.empty()) h.emplace("Authorization", "Bearer " + cfg_.apiKey);
            auto res = cli.Post(cfg_.path, h, body.dump(), "application/json");
            if (!res) {
                out.error = "image server request failed: " + httplib::to_string(res.error());
                out.unreachable = res.error() == httplib::Error::Connection || res.error() == httplib::Error::ConnectionTimeout;
                return out;
            }
            if (res->status < 200 || res->status >= 300) { out.error = Cat("image server HTTP ", res->status, ": ", res->body.substr(0, 200)); return out; }
            const Json j = Json::parse(res->body, nullptr, false);
            if (j.is_discarded() || !j.contains("data") || !j["data"].is_array() || j["data"].empty() || !j["data"][0].contains("b64_json") || !j["data"][0]["b64_json"].is_string()) {
                out.error = "image server sent no b64_json image";
                return out;
            }
            out.bytes = Base64Decode(j["data"][0]["b64_json"].get<std::string>());
            out.ok = !out.bytes.empty();
            if (!out.ok) out.error = "image server sent an empty image";
        } catch (const std::exception& e) { out.error = e.what(); }
        return out;
    }
    Json Probe() override {
        Json p{{"reachable", false}, {"models", Json::array()}};
        try {
            httplib::Client cli(cfg_.url);
            cli.set_connection_timeout(2);
            cli.set_read_timeout(5);
            httplib::Headers h;
            if (!cfg_.apiKey.empty()) h.emplace("Authorization", "Bearer " + cfg_.apiKey);
            auto res = cli.Get("/v1/models", h);
            if (!res) { p["error"] = "cannot reach " + cfg_.url + " (" + httplib::to_string(res.error()) + ")"; return p; }
            p["reachable"] = res->status < 500;
            const Json j = Json::parse(res->body, nullptr, false);
            if (j.is_object() && j.contains("data") && j["data"].is_array())
                for (const auto& m : j["data"]) p["models"].push_back(m.value("id", ""));
        } catch (const std::exception& e) { p["error"] = e.what(); }
        return p;
    }
    std::string Describe() const override { return "openai images at " + cfg_.url; }

private:
    ImageConfig cfg_;
};

}  // namespace

// ---------------------------------------------------------------- configuration

std::optional<ImageConfig> ImageConfig::Load(const std::string& imagesFile, const std::string& styleFile, const std::string& promptsFile, std::string& err) {
    ImageConfig c;
    Json j;
    std::string e;
    try {
        if (ReadJsonFile(imagesFile, j, e)) {
            if (!j.is_object()) { err = imagesFile + ": must be a JSON object"; return std::nullopt; }
            c.enabled = j.value("enabled", c.enabled);
            c.backend = j.value("backend", c.backend);
            c.url = j.value("url", c.url);
            c.path = j.value("path", c.path);
            c.apiKey = j.value("apiKey", c.apiKey);
            c.checkpoint = j.value("checkpoint", c.checkpoint);
            c.requireStore = j.value("requireStore", c.requireStore);
            c.timeoutSec = std::max(5, j.value("timeoutSec", c.timeoutSec));
        } else if (!e.empty()) { err = e; return std::nullopt; }
        if (c.backend != "a1111" && c.backend != "openai") { err = "images backend must be \"a1111\" or \"openai\", not \"" + c.backend + "\""; return std::nullopt; }

        Json s;
        if (ReadJsonFile(styleFile, s, e)) {
            ImageStyle& st = c.style;
            st.prefix = s.value("prefix", st.prefix);
            st.suffix = s.value("suffix", st.suffix);
            st.negative = s.value("negative", st.negative);
            st.steps = std::clamp(s.value("steps", st.steps), 1, 150);
            st.cfg = s.value("cfg", st.cfg);
            st.sampler = s.value("sampler", st.sampler);
            if (s.contains("framing") && s["framing"].is_object())
                for (auto it = s["framing"].begin(); it != s["framing"].end(); ++it)
                    if (it.value().is_string()) st.framing[it.key()] = it.value().get<std::string>();
            auto size = [&](const char* k, int& w, int& h) {
                if (s.contains(k) && s[k].is_array() && s[k].size() == 2) { w = std::clamp(s[k][0].get<int>(), 64, 2048); h = std::clamp(s[k][1].get<int>(), 64, 2048); }
            };
            size("portrait", st.portraitW, st.portraitH);
            size("wide", st.wideW, st.wideH);
            if (c.checkpoint.empty()) c.checkpoint = s.value("checkpoint", "");
        } else if (!e.empty()) { err = e; return std::nullopt; }

        Json p;
        if (ReadJsonFile(promptsFile, p, e)) {
            if (!p.is_object()) { err = promptsFile + ": must be a JSON object of \"kind/id\": \"subject\""; return std::nullopt; }
            for (auto it = p.begin(); it != p.end(); ++it)
                if (!it.key().empty() && it.key()[0] != '_' && it.value().is_string()) c.prompts[it.key()] = it.value().get<std::string>();
        } else if (!e.empty()) { err = e; return std::nullopt; }
    } catch (const Json::exception& ex) { err = std::string("bad image settings: ") + ex.what(); return std::nullopt; }
    return c;
}

// ---------------------------------------------------------------- subjects

std::uint32_t Crc32(const std::string& s) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    for (unsigned char ch : s) c = table[(c ^ ch) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::vector<ArtSubject> ArtSubjects(const std::map<std::string, std::string>& overrides) {
    static const std::set<std::string> creatureTypes = {"humanoid", "undead", "beast", "giant", "fiend", "celestial", "construct", "dragon", "elemental", "fey", "monstrosity", "ooze", "plant", "aberration"};
    std::vector<ArtSubject> out;
    std::set<std::string> seen;
    auto add = [&](const std::string& kind, const std::string& id, std::string subject, bool wide) {
        if (id.empty() || !seen.insert(kind + "/" + id).second) return;
        bool custom = false;
        if (auto it = overrides.find(kind + "/" + id); it != overrides.end()) { subject = it->second; custom = true; }
        out.push_back(ArtSubject{kind, id, std::move(subject), wide, custom});
    };
    for (const auto& c : Content::Characters()) {
        std::vector<std::string> race;
        for (const auto& t : c.tags) if (!creatureTypes.count(t)) race.push_back(t);
        if (race.empty()) race = {"human"};
        add("characters", c.id, Cat(c.name.empty() ? c.id : c.name, ", ", Article(Join(race)), " ", Join(race), " ", c.classId.empty() ? "adventurer" : c.classId, ", adventurer"), false);
    }
    for (const auto& m : Content::Monsters())
        add("monsters", m.id, Cat(LowerCase(m.name.empty() ? m.id : m.name), ", ", Join(m.sheet.tags), " monster, dungeons and dragons creature, menacing"), false);
    for (const auto& mod : Content::Modules()) {
        add("modules", mod.id, Cat(mod.name.empty() ? mod.id : mod.name, ", ", mod.setting), true);
        for (const auto& loc : mod.locations) {
            add("locations", loc.id, Cat(loc.name.empty() ? loc.id : loc.name, ", ", loc.kind, ", ", loc.description), true);
            for (const auto& n : loc.npcs)
                add("npcs", n.id, Cat(n.name.empty() ? n.id : n.name, ", ", Article(n.type), " ", n.type, ". ", n.description), false);
        }
    }
    return out;
}

std::shared_ptr<IImageClient> MakeImageClient(const ImageConfig& cfg) {
    if (cfg.backend == "openai") return std::make_shared<OpenAiImageClient>(cfg);
    return std::make_shared<A1111Client>(cfg);
}

// ---------------------------------------------------------------- service

ArtService::ArtService(ImageConfig cfg, std::string artDir, std::shared_ptr<IImageClient> client)
    : cfg_(std::move(cfg)), artDir_(std::move(artDir)), client_(client ? std::move(client) : MakeImageClient(cfg_)) {
    worker_ = std::thread([this] { Worker(); });
}

ArtService::~ArtService() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

std::string ArtService::FindWeights(const std::string& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return "";
    for (const auto& e : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        if (!e.is_regular_file(ec)) continue;
        const std::string ext = LowerCase(e.path().extension().string());
        if (ext == ".safetensors" || ext == ".ckpt" || ext == ".gguf") return e.path().string();
    }
    return "";
}

// The name the image server is told to load. A model in the store resolves to its weights file's name.
std::string ArtService::EffectiveCheckpoint(std::string& problem) const {
    problem.clear();
    if (cfg_.checkpoint.empty()) return "";
    const ModelInfo mi = LocateModel(cfg_.checkpoint);
    if (mi.exists) {
        const std::string w = FindWeights(mi.path);
        if (w.empty()) { problem = Cat("model '", cfg_.checkpoint, "' is in the model store (", mi.path, ") but holds no .safetensors, .ckpt or .gguf file"); return ""; }
        return cfg_.backend == "a1111" ? fs::path(w).filename().string() : cfg_.checkpoint;
    }
    if (cfg_.requireStore) { problem = Cat("model '", cfg_.checkpoint, "' is not in the model store (", mi.path, ")"); return ""; }
    return cfg_.checkpoint;  // a name the image server knows by itself
}

Json ArtService::Status() {
    ImageConfig cfg;
    bool busy;
    long long at;
    Json probe;
    {
        std::lock_guard<std::mutex> lk(m_);
        cfg = cfg_;
        busy = busy_;
        at = probeAt_;
        probe = probe_;
    }
    if (cfg.enabled && (at == 0 || (!busy && NowMs() - at > 5000))) {
        probe = client_->Probe();  // not while a picture is being drawn: the server may be too busy to answer
        std::lock_guard<std::mutex> lk(m_);
        probe_ = probe;
        probeAt_ = NowMs();
    }
    std::string problem;
    const std::string eff = EffectiveCheckpoint(problem);
    Json storeModels = Json::array();
    for (const auto& m : ListModels())
        if (!FindWeights(m.path).empty()) storeModels.push_back(m.name);
    const ModelInfo mi = LocateModel(cfg.checkpoint);
    Json s{{"enabled", cfg.enabled}, {"backend", cfg.backend}, {"url", cfg.url}, {"describe", client_->Describe()},
           {"reachable", probe.value("reachable", false)}, {"checkpoint", cfg.checkpoint}, {"effectiveCheckpoint", eff},
           {"requireStore", cfg.requireStore}, {"store", Json{{"home", mi.home}, {"path", mi.path}, {"exists", mi.exists}}},
           {"models", Json{{"store", storeModels}, {"server", probe.contains("models") ? probe["models"] : Json::array()}}}};
    if (probe.contains("error")) s["error"] = probe["error"];
    if (!problem.empty()) s["problem"] = problem;
    s["jobs"] = Jobs();
    return s;
}

bool ArtService::SetCheckpoint(const std::string& model, bool fromStore, std::string& err) {
    if (fromStore && !model.empty()) {
        const ModelInfo mi = LocateModel(model);
        if (!mi.exists) { err = Cat("model '", model, "' is not in the model store (", mi.path, ")"); return false; }
        if (FindWeights(mi.path).empty()) { err = Cat("model '", model, "' holds no .safetensors, .ckpt or .gguf file in ", mi.path); return false; }
    }
    std::lock_guard<std::mutex> lk(m_);
    cfg_.checkpoint = model;
    return true;
}

std::string ArtService::Existing(const std::string& kind, const std::string& id) const {
    for (const char* ext : kExts) {
        const fs::path p = fs::path(artDir_) / kind / (id + "." + ext);
        std::error_code ec;
        if (fs::is_regular_file(p, ec)) return p.string();
    }
    return "";
}

Json ArtService::Enqueue(const Json& req, std::string& err) {
    ImageConfig cfg;
    {
        std::lock_guard<std::mutex> lk(m_);
        cfg = cfg_;
    }
    if (!cfg.enabled) { err = "image generation is turned off (images.json: \"enabled\")"; return Json(); }
    std::string problem;
    EffectiveCheckpoint(problem);
    if (!problem.empty()) { err = problem; return Json(); }

    const bool force = req.value("force", false);
    const auto all = ArtSubjects(cfg.prompts);
    std::vector<Item> want;
    auto find = [&](const std::string& kind, const std::string& id) -> const ArtSubject* {
        for (const auto& s : all) if (s.kind == kind && s.id == id) return &s;
        return nullptr;
    };
    if (req.value("missing", false)) {
        std::set<std::string> kinds;
        if (req.contains("kinds") && req["kinds"].is_array()) for (const auto& k : req["kinds"]) if (k.is_string()) kinds.insert(k.get<std::string>());
        for (const auto& s : all)
            if ((kinds.empty() || kinds.count(s.kind)) && (force || Existing(s.kind, s.id).empty())) want.push_back(Item{s, force, ""});
    }
    if (req.contains("ids") && req["ids"].is_array())
        for (const auto& v : req["ids"]) {
            const std::string key = v.is_string() ? v.get<std::string>() : "";
            const auto slash = key.find('/');
            const ArtSubject* s = slash == std::string::npos ? nullptr : find(key.substr(0, slash), key.substr(slash + 1));
            if (!s) { err = "unknown picture '" + key + "' (use kind/id, for example characters/thorin)"; return Json(); }
            want.push_back(Item{*s, force, ""});
        }
    if (req.contains("kind") && req.contains("id")) {
        const ArtSubject* s = find(req.value("kind", ""), req.value("id", ""));
        if (!s) { err = "unknown picture '" + req.value("kind", "") + "/" + req.value("id", "") + "'"; return Json(); }
        want.push_back(Item{*s, force, req.value("prompt", "")});
    }
    if (want.empty() && !req.value("missing", false) && !req.contains("ids") && !req.contains("kind")) { err = "say what to draw: {\"missing\": true}, {\"ids\": [\"characters/thorin\"]} or {\"kind\": \"npcs\", \"id\": \"marta\"}"; return Json(); }

    int queued = 0, skipped = 0;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!busy_ && queue_.empty()) { done_ = failed_ = 0; errors_.clear(); recent_.clear(); }
        for (auto& it : want) {
            const std::string key = it.s.kind + "/" + it.s.id;
            bool dup = current_ == key && busy_;
            for (const auto& q : queue_) dup = dup || (q.s.kind == it.s.kind && q.s.id == it.s.id);
            if (dup || (!it.force && !Existing(it.s.kind, it.s.id).empty())) { ++skipped; continue; }
            queue_.push_back(std::move(it));
            ++queued;
        }
    }
    cv_.notify_all();
    return Json{{"queued", queued}, {"skipped", skipped}};
}

Json ArtService::Jobs() const {
    std::lock_guard<std::mutex> lk(m_);
    return Json{{"running", busy_ || !queue_.empty()}, {"current", busy_ ? current_ : ""}, {"queued", queue_.size()}, {"done", done_}, {"failed", failed_},
                {"total", static_cast<std::size_t>(done_ + failed_) + queue_.size() + (busy_ ? 1 : 0)}, {"errors", errors_}, {"recent", recent_}};
}

void ArtService::Cancel() {
    std::lock_guard<std::mutex> lk(m_);
    queue_.clear();
    idle_.notify_all();
}

void ArtService::WaitIdle() {
    std::unique_lock<std::mutex> lk(m_);
    idle_.wait(lk, [&] { return queue_.empty() && !busy_; });
}

void ArtService::Worker() {
    for (;;) {
        Item it;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            it = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
            current_ = it.s.kind + "/" + it.s.id;
        }
        std::string err;
        const bool ok = Do(it, err);
        {
            std::lock_guard<std::mutex> lk(m_);
            if (ok) {
                ++done_;
                recent_.push_back(current_);
                if (recent_.size() > 20) recent_.erase(recent_.begin());
            } else {
                ++failed_;
                errors_.push_back(current_ + ": " + err);
                if (errors_.size() > 20) errors_.erase(errors_.begin());
                if (err.rfind("unreachable:", 0) == 0) {  // no point asking a dead server for the rest
                    failed_ += static_cast<int>(queue_.size());
                    errors_.push_back(Cat(queue_.size(), " more picture(s) dropped"));
                    queue_.clear();
                }
            }
            busy_ = false;
            current_.clear();
            if (queue_.empty()) idle_.notify_all();
        }
    }
}

bool ArtService::Do(const Item& it, std::string& err) {
    ImageConfig cfg;
    {
        std::lock_guard<std::mutex> lk(m_);
        cfg = cfg_;
    }
    if (!it.force && !Existing(it.s.kind, it.s.id).empty()) return true;
    std::string problem;
    const std::string eff = EffectiveCheckpoint(problem);
    if (!problem.empty()) { err = problem; return false; }

    ImageRequest r;
    const ImageStyle& st = cfg.style;
    const std::string subject = it.promptOverride.empty() ? it.s.subject : it.promptOverride;
    std::string framing;
    if (!it.s.custom && it.promptOverride.empty())
        if (auto f = st.framing.find(it.s.kind); f != st.framing.end()) framing = f->second;
    for (const std::string& part : {st.prefix, subject, framing, st.suffix}) {  // comma-separated phrases, as Stable Diffusion likes them
        if (part.empty()) continue;
        if (!r.prompt.empty()) r.prompt += (r.prompt.back() == ',' || r.prompt.back() == '.') ? " " : ", ";
        r.prompt += part;
    }
    r.negative = st.negative;
    r.sampler = st.sampler;
    r.steps = st.steps;
    r.cfg = st.cfg;
    r.width = it.s.wide ? st.wideW : st.portraitW;
    r.height = it.s.wide ? st.wideH : st.portraitH;
    r.seed = Crc32(it.s.id) & 0x7FFFFFFFu;
    r.checkpoint = eff;

    const ImageResult res = client_->Generate(r);
    if (!res.ok) { err = (res.unreachable ? "unreachable: " : "") + res.error; return false; }
    const std::string ext = Sniff(res.bytes);
    if (ext.empty()) { err = "the image server did not send a PNG, JPEG or WebP picture"; return false; }

    std::error_code ec;
    const fs::path dir = fs::path(artDir_) / it.s.kind;
    fs::create_directories(dir, ec);
    const fs::path final = dir / (it.s.id + "." + ext), tmp = dir / (it.s.id + "." + ext + ".part");
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) { err = "cannot write " + tmp.string(); return false; }
        out.write(res.bytes.data(), static_cast<std::streamsize>(res.bytes.size()));
        if (!out) { err = "cannot write " + tmp.string(); return false; }
    }
    for (const char* e : kExts) {  // one picture per id: remove an older one in another format
        const fs::path other = dir / (it.s.id + "." + e);
        if (other != final) fs::remove(other, ec);
    }
    fs::rename(tmp, final, ec);
    if (ec) { fs::remove(final, ec); ec.clear(); fs::rename(tmp, final, ec); }
    if (ec) { err = "cannot save " + final.string() + ": " + ec.message(); return false; }
    return true;
}

}  // namespace dnd
