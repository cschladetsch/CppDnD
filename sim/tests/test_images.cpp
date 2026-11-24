#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#include "dnd/AI.hpp"
#include "dnd/Agents.hpp"
#include "dnd/Combat.hpp"
#include "dnd/Content.hpp"
#include "dnd/DM.hpp"
#include "dnd/Game.hpp"
#include "dnd/Human.hpp"
#include "dnd/Images.hpp"
#include "dnd/Json.hpp"
#include "dnd/Llm.hpp"
#include "dnd/Scene.hpp"
#include "test_support.hpp"

using namespace dnd;


TEST(Images, ServiceAndConfig) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "dnd_images_test";
    fs::remove_all(root);
    const std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + "fakepixels";

    // ---- subjects come from the loaded content, with stable seeds and overridable prompts
    const auto subjects = ArtSubjects();
    EXPECT_FALSE(subjects.empty());
    auto has = [&](const std::string& k, const std::string& id) { for (const auto& s : subjects) if (s.kind == k && s.id == id) return true; return false; };
    EXPECT_TRUE(has("characters", "thorin") && has("monsters", "zombie") && has("modules", "ravenmoor") && has("npcs", "marta"));
    EXPECT_TRUE(Crc32("thorin") == Crc32("thorin") && Crc32("thorin") != Crc32("elara") && Crc32("123456789") == 0xCBF43926u);
    const auto over = ArtSubjects({{"characters/thorin", "a dwarf in plate"}});
    for (const auto& s : over) if (s.id == "thorin" && s.kind == "characters") EXPECT_EQ(s.subject, "a dwarf in plate");

    // ---- configuration files
    {
        fs::create_directories(root / "cfg");
        std::ofstream(root / "cfg" / "images.json") << R"({ /* comment */ "backend": "openai", "url": "http://x:1", "checkpoint": "mymodel", "requireStore": true })";
        std::ofstream(root / "cfg" / "style.json") << R"({"prefix": "PRE", "steps": 7, "portrait": [256, 320], "wide": [640, 360], "framing": {"monsters": "side view"}})";
        std::ofstream(root / "cfg" / "prompts.json") << R"({"_note": "x", "monsters/zombie": "a rotting corpse"})";
        std::string err;
        auto c = ImageConfig::Load((root / "cfg" / "images.json").string(), (root / "cfg" / "style.json").string(), (root / "cfg" / "prompts.json").string(), err);
        EXPECT_TRUE(c && c->backend == "openai" && c->url == "http://x:1" && c->requireStore && c->style.prefix == "PRE" && c->style.steps == 7 && c->style.portraitH == 320 && c->style.framing.at("monsters") == "side view" && c->style.framing.count("characters") == 1);
        EXPECT_TRUE(c && c->prompts.size() == 1 && c->prompts.at("monsters/zombie") == "a rotting corpse");
        std::ofstream(root / "cfg" / "bad.json") << R"({"backend": "dalle"})";
        EXPECT_TRUE(!ImageConfig::Load((root / "cfg" / "bad.json").string(), "", "", err) && err.find("backend") != std::string::npos);
        auto none = ImageConfig::Load("", "", "", err);  // every file may be missing
        EXPECT_TRUE(none && none->backend == "a1111" && none->enabled);
    }

    // ---- the service draws what is missing, once, in the background
    {
        std::mutex mu;
        std::vector<ImageRequest> seen;
        auto client = std::make_shared<MockImageClient>([&](const ImageRequest& r) {
            std::lock_guard<std::mutex> lk(mu);
            seen.push_back(r);
            ImageResult out;
            out.ok = true;
            out.bytes = png;
            return out;
        });
        ImageConfig cfg;
        cfg.style.prefix = "STYLE";
        cfg.prompts["characters/thorin"] = "custom thorin";
        ArtService svc(cfg, (root / "art").string(), client);
        std::string err;
        Json q = svc.Enqueue(Json{{"ids", Json::array({"characters/thorin", "monsters/zombie"})}}, err);
        EXPECT_TRUE(err.empty() && q["queued"] == 2);
        svc.WaitIdle();
        EXPECT_TRUE(fs::exists(root / "art" / "characters" / "thorin.png") && fs::exists(root / "art" / "monsters" / "zombie.png"));
        {
            std::lock_guard<std::mutex> lk(mu);
            EXPECT_TRUE(seen.size() == 2 && seen[0].prompt == "STYLE, custom thorin" && seen[0].seed == (Crc32("thorin") & 0x7FFFFFFFu) && seen[0].width == 512 && seen[0].height == 512);
            EXPECT_TRUE(seen[1].prompt.rfind("STYLE, ", 0) == 0 && seen[1].prompt.find("zombie") != std::string::npos);
            EXPECT_NE(seen[1].prompt.find("full body, three-quarter view"), std::string::npos);  // framing for creatures
        }
        EXPECT_FALSE(fs::exists(root / "art" / "characters" / "thorin.png.part"));
        // nothing is redrawn unless forced; "missing" skips what exists
        q = svc.Enqueue(Json{{"ids", Json::array({"characters/thorin"})}}, err);
        EXPECT_TRUE(q["queued"] == 0 && q["skipped"] == 1);
        q = svc.Enqueue(Json{{"missing", true}, {"kinds", Json::array({"characters"})}}, err);
        svc.WaitIdle();
        std::size_t chars = 0;
        for (const auto& s : subjects) chars += s.kind == "characters" ? 1 : 0;
        EXPECT_TRUE(q["queued"].get<std::size_t>() == chars - 1);
        q = svc.Enqueue(Json{{"kind", "characters"}, {"id", "thorin"}, {"force", true}, {"prompt", "once more"}}, err);
        svc.WaitIdle();
        {
            std::lock_guard<std::mutex> lk(mu);
            EXPECT_TRUE(q["queued"] == 1 && seen.back().prompt == "STYLE, once more");
        }
        const Json jobs = svc.Jobs();
        EXPECT_TRUE(!jobs["running"].get<bool>() && jobs["failed"] == 0 && jobs["done"].get<int>() >= 1);
        // bad requests are refused with a reason
        svc.Enqueue(Json{{"ids", Json::array({"characters/nobody"})}}, err);
        EXPECT_NE(err.find("unknown picture"), std::string::npos);
        err.clear();
        svc.Enqueue(Json::object(), err);
        EXPECT_FALSE(err.empty());
        // wide pictures use the wide size, and a JPEG is kept as a JPEG replacing the PNG
        err.clear();
        svc.Enqueue(Json{{"ids", Json::array({"modules/ravenmoor"})}}, err);
        svc.WaitIdle();
        EXPECT_TRUE(seen.back().width == 768 && seen.back().height == 432 && seen.back().prompt.find("wide establishing") == std::string::npos);
        EXPECT_NE(seen.back().prompt.find("panoramic establishing shot"), std::string::npos);  // module banners are framed as panoramas
    }

    // ---- failures are reported per picture; a dead server drops the rest of the queue
    {
        auto bad = std::make_shared<MockImageClient>([](const ImageRequest&) { ImageResult r; r.error = "boom"; return r; });
        ArtService svc(ImageConfig{}, (root / "art2").string(), bad);
        std::string err;
        svc.Enqueue(Json{{"ids", Json::array({"characters/thorin", "characters/elara"})}}, err);
        svc.WaitIdle();
        Json j = svc.Jobs();
        EXPECT_TRUE(j["failed"] == 2 && j["errors"].size() == 2 && j["errors"][0].get<std::string>().find("boom") != std::string::npos);
        EXPECT_FALSE(fs::exists(root / "art2" / "characters" / "thorin.png"));

        auto dead = std::make_shared<MockImageClient>([](const ImageRequest&) { ImageResult r; r.error = "refused"; r.unreachable = true; return r; });
        ArtService svc2(ImageConfig{}, (root / "art3").string(), dead);
        svc2.Enqueue(Json{{"missing", true}}, err);
        svc2.WaitIdle();
        j = svc2.Jobs();
        EXPECT_TRUE(j["failed"].get<std::size_t>() == subjects.size() && j["queued"] == 0);

        auto notAnImage = std::make_shared<MockImageClient>([](const ImageRequest&) { ImageResult r; r.ok = true; r.bytes = "<html>oops</html>"; return r; });
        ArtService svc3(ImageConfig{}, (root / "art4").string(), notAnImage);
        svc3.Enqueue(Json{{"ids", Json::array({"characters/thorin"})}}, err);
        svc3.WaitIdle();
        EXPECT_TRUE(svc3.Jobs()["failed"] == 1 && !fs::exists(root / "art4" / "characters" / "thorin.png"));
    }

    // ---- the model comes from the model store when it is there
    {
        const fs::path home = root / "models";
        fs::create_directories(home / "sd15");
        std::ofstream(home / "sd15" / "v1-5-pruned.safetensors") << "weights";
        fs::create_directories(home / "empty");
        fs::create_directories(home / "textmodel");
        std::ofstream(home / "textmodel" / "llama.gguf") << "g";
#ifdef _WIN32
        _putenv_s("DEEPSEEK_MODEL_HOME", home.string().c_str());
#else
        setenv("DEEPSEEK_MODEL_HOME", home.string().c_str(), 1);
#endif
        std::string sent;
        auto client = std::make_shared<MockImageClient>([&](const ImageRequest& r) { sent = r.checkpoint; ImageResult o; o.ok = true; o.bytes = png; return o; });
        ImageConfig cfg;
        cfg.checkpoint = "sd15";
        ArtService svc(cfg, (root / "art5").string(), client);
        std::string err;
        svc.Enqueue(Json{{"ids", Json::array({"characters/thorin"})}}, err);
        svc.WaitIdle();
        EXPECT_EQ(sent, "v1-5-pruned.safetensors");  // the store's folder resolved to its weights file
        Json st = svc.Status();
        EXPECT_TRUE(st["store"]["exists"] == true && st["effectiveCheckpoint"] == "v1-5-pruned.safetensors" && st["reachable"] == true);
        bool listed = false;
        for (const auto& m : st["models"]["store"]) listed = listed || m == "sd15";
        EXPECT_TRUE(listed);
        // switching: a name the server knows is accepted; from the store it must exist and hold weights
        EXPECT_TRUE(svc.SetCheckpoint("someOtherModel.ckpt", false, err));
        EXPECT_TRUE(!svc.SetCheckpoint("nope", true, err) && err.find("model store") != std::string::npos);
        EXPECT_TRUE(!svc.SetCheckpoint("empty", true, err) && err.find("holds no") != std::string::npos);
        EXPECT_TRUE(svc.SetCheckpoint("sd15", true, err));
        // mature mode naming a checkpoint that is neither in the store nor on the server (the shipped default before it is
        // downloaded) draws with the base checkpoint and says so; once the server lists it, it is used as it is
        {
            struct Listing : MockImageClient {
                Json models;
                using MockImageClient::MockImageClient;
                Json Probe() override { return Json{{"reachable", true}, {"models", models}}; }
            };
            auto lc = std::make_shared<Listing>([&](const ImageRequest& r) { sent = r.checkpoint; ImageResult o; o.ok = true; o.bytes = png; return o; });
            lc->models = Json::array({"v1-5-pruned.safetensors"});
            ImageConfig mc;
            mc.checkpoint = "sd15";
            mc.mature = true;
            mc.matureStyle.checkpoint = "realistic-vision-5.1";
            ArtService ms(mc, (root / "art5m").string(), lc);
            Json mst = ms.Status();
            EXPECT_EQ(mst["effectiveCheckpoint"], "v1-5-pruned.safetensors");
            EXPECT_NE(mst.value("note", "").find("realistic-vision-5.1"), std::string::npos);
            lc->models = Json::array({"v1-5-pruned.safetensors", "realistic-vision-5.1"});
            ArtService ms2(mc, (root / "art5n").string(), lc);
            EXPECT_EQ(ms2.Status()["effectiveCheckpoint"], "realistic-vision-5.1");
            // the same in normal mode: the shipped realistic-vision-5.1 before it is downloaded falls back to sd15
            lc->models = Json::array({"v1-5-pruned.safetensors"});
            ImageConfig nc;
            nc.checkpoint = "realistic-vision-5.1";
            ArtService ns(nc, (root / "art5o").string(), lc);
            Json nst = ns.Status();
            EXPECT_EQ(nst["effectiveCheckpoint"], "v1-5-pruned.safetensors");
            EXPECT_NE(nst.value("note", "").find("realistic-vision-5.1"), std::string::npos);
            lc->models = Json::array({"v1-5-pruned.safetensors", "realistic-vision-5.1.safetensors"});
            ArtService ns2(nc, (root / "art5p").string(), lc);
            EXPECT_EQ(ns2.Status()["effectiveCheckpoint"], "realistic-vision-5.1");
        }
        // requireStore refuses an unknown model before anything is drawn
        ImageConfig strict;
        strict.checkpoint = "not-in-store";
        strict.requireStore = true;
        ArtService svc2(strict, (root / "art6").string(), client);
        svc2.Enqueue(Json{{"ids", Json::array({"characters/thorin"})}}, err = "");
        EXPECT_TRUE(err.find("not in the model store") != std::string::npos && svc2.Status().contains("problem"));
        ImageConfig off;
        off.enabled = false;
        ArtService svc3(off, (root / "art7").string(), client);
        err.clear();
        svc3.Enqueue(Json{{"missing", true}}, err);
        EXPECT_NE(err.find("turned off"), std::string::npos);
    }
    fs::remove_all(root);
}

// A module's own look (the art block in its JSON) words its pictures, so a starship is not drawn as a forest.
TEST(Images, ModuleArtStyleReplacesTheGlobalWords) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "dnd_images_modart";
    fs::remove_all(root);
    const std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + "fakepixels";
    std::mutex mu;
    std::map<std::string, ImageRequest> seen;  // by the id's seed
    std::vector<ImageRequest> all;
    auto client = std::make_shared<MockImageClient>([&](const ImageRequest& r) {
        std::lock_guard<std::mutex> lk(mu);
        all.push_back(r);
        ImageResult o; o.ok = true; o.bytes = png; return o;
    });
    ImageConfig cfg;
    cfg.style.prefix = "GLOBALSTYLE";
    cfg.style.negative = "GLOBALNEG";
    ArtService svc(cfg, root.string(), client);
    std::string err;
    svc.Enqueue(Json{{"ids", Json::array({"locations/lifeboat_cradle", "locations/greywick", "monsters/husk", "monsters/zombie", "characters/cmdr_reyes"})}}, err);
    svc.WaitIdle();
    ASSERT_EQ(all.size(), 5u) << err;
    auto find = [&](const std::string& word) { for (const auto& r : all) if (r.prompt.find(word) != std::string::npos) return r; ADD_FAILURE() << word; return ImageRequest{}; };
    const ImageRequest ship = find("Boarding Shuttle Cradle"), town = find("Greywick"), husk = find("husk"), zombie = find("zombie"), hero = find("Commander Reyes");
    for (const auto& r : {ship, husk, hero}) {
        EXPECT_NE(r.prompt.find("starship"), std::string::npos) << r.prompt;
        EXPECT_EQ(r.prompt.find("GLOBALSTYLE"), std::string::npos) << r.prompt;
        EXPECT_NE(r.negative.find("forest"), std::string::npos);
        EXPECT_EQ(r.negative.find("GLOBALNEG"), std::string::npos);
    }
    EXPECT_NE(husk.prompt.find("alien"), std::string::npos);
    EXPECT_EQ(husk.prompt.find("dungeons and dragons"), std::string::npos);
    EXPECT_EQ(town.prompt.find("GLOBALSTYLE"), 0u);       // Ravenmoor has no art block: the global words stay
    EXPECT_EQ(zombie.prompt.find("GLOBALSTYLE"), 0u);
    EXPECT_NE(zombie.prompt.find("dungeons and dragons"), std::string::npos);

    // camera views take the style of the game's module
    all.clear();
    ViewJob v; v.id = "v1"; v.subject = "a corridor"; v.module = "meridian";
    svc.EnqueueView(v, err);
    ViewJob w; w.id = "v2"; w.subject = "a lane"; w.module = "ravenmoor";
    svc.EnqueueView(w, err);
    svc.WaitIdle();
    ASSERT_EQ(all.size(), 2u);
    for (const auto& r : all) {
        if (r.prompt.find("corridor") != std::string::npos) EXPECT_NE(r.prompt.find("starship"), std::string::npos);
        else EXPECT_EQ(r.prompt.find("GLOBALSTYLE"), 0u);
    }
    fs::remove_all(root);
}

// ---------------------------------------------------------------- mature mode

namespace {
ImageResult FakePng(const ImageRequest&) {
    ImageResult out;
    out.ok = true;
    out.bytes = std::string("\x89PNG\r\n\x1a\n", 8) + "fakepixels";
    return out;
}
const ArtSubject& SubjectOf(const std::vector<ArtSubject>& all, const std::string& kind, const std::string& id) {
    for (const auto& s : all) if (s.kind == kind && s.id == id) return s;
    throw std::runtime_error("no subject " + kind + "/" + id);
}
}  // namespace

TEST(MatureImages, MinorWordsAreFound) {
    for (const char* t : {"a child", "Two KIDS play", "teenager", "a 16 year old", "16-year-old", "15yo", "aged 12", "schoolgirl", "loli", "1girl, solo", "a young boy"})
        EXPECT_TRUE(MentionsMinor(t)) << t;
    for (const char* t : {"Elara, an adult woman, an elf wizard", "a 30 years old knight", "kidney stones", "minority", "he lived here 3 years",
                          "a 117 year old elf", "childhood memories", "a young man", "a ruined chapel"})
        EXPECT_FALSE(MentionsMinor(t)) << t;
    EXPECT_EQ(AdultWords("female"), "adult woman");
    EXPECT_EQ(AdultWords("Male"), "adult man");
    EXPECT_EQ(AdultWords(""), "adult");
    EXPECT_EQ(AdultWords("nonbinary"), "adult");
}

TEST(MatureImages, ConfigLoadsAndIsOffByDefault) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "dnd_mature_images_cfg";
    fs::remove_all(root);
    fs::create_directories(root);
    std::string err;
    auto none = ImageConfig::Load("", "", "", err);
    ASSERT_TRUE(none);
    EXPECT_FALSE(none->mature);
    std::ofstream(root / "images.json") << R"({"checkpoint": "sd15", "mature": {"checkpoint": "grown", "negative": "NEG", "steps": 30, "portrait": [832, 1216], "framing": {"characters": "FULL"}}})";
    auto c = ImageConfig::Load((root / "images.json").string(), "", "", err);
    ASSERT_TRUE(c) << err;
    EXPECT_FALSE(c->mature);  // the block alone does not turn it on
    EXPECT_EQ(c->ActiveCheckpoint(), "sd15");
    c->mature = true;
    EXPECT_EQ(c->ActiveCheckpoint(), "grown");
    EXPECT_TRUE(c->matureStyle.negative == "NEG" && c->matureStyle.steps == 30 && c->matureStyle.portraitW == 832 && c->matureStyle.framing.at("characters") == "FULL");
    std::ofstream(root / "on.json") << R"({"mature": {"enabled": true}})";
    auto on = ImageConfig::Load((root / "on.json").string(), "", "", err);
    EXPECT_TRUE(on && on->mature && on->ActiveCheckpoint().empty());
    std::ofstream(root / "bad.json") << R"({"mature": true})";
    EXPECT_FALSE(ImageConfig::Load((root / "bad.json").string(), "", "", err));
}

TEST(MatureImages, OffChangesNothing) {
    ImageConfig cfg;
    cfg.matureStyle.prefix = "MATURE";
    cfg.matureStyle.negative = "MNEG";
    ArtService svc(cfg, "", std::make_shared<MockImageClient>(FakePng));
    const auto all = ArtSubjects();
    ImageRequest r;
    std::string err;
    ASSERT_TRUE(svc.BuildRequest(SubjectOf(all, "characters", "elara"), "", nullptr, r, err));
    EXPECT_EQ(r.prompt.find("MATURE"), std::string::npos);
    EXPECT_EQ(r.prompt.find("adult"), std::string::npos);
    EXPECT_EQ(r.negative, cfg.style.negative + ", " + cfg.style.sfwNegative);  // outside mature mode nudity is kept out
    EXPECT_EQ(r.negative.find("MNEG"), std::string::npos);
    ASSERT_TRUE(svc.BuildRequest(SubjectOf(all, "characters", "elara"), "a child", nullptr, r, err));  // the guard is a mature-mode guard
    ImageConfig m = cfg;
    m.mature = true;
    ArtService msvc(m, "", std::make_shared<MockImageClient>(FakePng));
    ASSERT_TRUE(msvc.BuildRequest(SubjectOf(all, "characters", "elara"), "", nullptr, r, err));
    EXPECT_EQ(r.negative.find("nsfw"), std::string::npos);  // mature mode leaves them out
}

TEST(MatureImages, WordsChecksAndTheGuard) {
    ImageConfig cfg;
    cfg.mature = true;
    cfg.checkpoint = "sd15";
    cfg.matureStyle.checkpoint = "grown";
    cfg.matureStyle.prefix = "MPRE";
    cfg.matureStyle.negative = "MNEG";
    cfg.matureStyle.framing["characters"] = "MFRAME";
    cfg.matureStyle.portraitW = 832;
    cfg.matureStyle.portraitH = 1216;
    ArtService svc(cfg, "", std::make_shared<MockImageClient>(FakePng));
    EXPECT_TRUE(svc.Mature());
    const auto all = ArtSubjects();
    ImageRequest r;
    std::string err;

    ASSERT_TRUE(svc.BuildRequest(SubjectOf(all, "characters", "elara"), "", nullptr, r, err)) << err;
    EXPECT_EQ(r.prompt.rfind("MPRE", 0), 0u);
    EXPECT_NE(r.prompt.find("Elara"), std::string::npos);
    EXPECT_NE(r.prompt.find("adult woman"), std::string::npos);
    EXPECT_NE(r.prompt.find("MFRAME"), std::string::npos);
    EXPECT_TRUE(r.width == 832 && r.height == 1216);
    // the guard's negative comes last, after the normal and the mature negatives
    EXPECT_EQ(r.negative.rfind(cfg.style.negative, 0), 0u);
    EXPECT_NE(r.negative.find("MNEG"), std::string::npos);
    EXPECT_EQ(r.negative.substr(r.negative.size() - AdultsOnlyNegative().size()), AdultsOnlyNegative());
    ASSERT_TRUE(svc.BuildRequest(SubjectOf(all, "characters", "thorin"), "", nullptr, r, err));
    EXPECT_NE(r.prompt.find("adult man"), std::string::npos);

    // a minor anywhere in the words, from the data, an override or the settings, is refused
    EXPECT_FALSE(svc.BuildRequest(SubjectOf(all, "characters", "elara"), "Elara as a teenager", nullptr, r, err));
    EXPECT_NE(err.find("adults"), std::string::npos);
    ArtSubject custom = SubjectOf(all, "npcs", "marta");
    custom.subject = "a little girl";
    custom.custom = true;
    EXPECT_FALSE(svc.BuildRequest(custom, "", nullptr, r, err));
    ImageConfig bad = cfg;
    bad.matureStyle.suffix = "1girl";
    ArtService svc2(bad, "", std::make_shared<MockImageClient>(FakePng));
    EXPECT_FALSE(svc2.BuildRequest(SubjectOf(all, "characters", "elara"), "", nullptr, r, err));

    // a camera view says everyone is an adult and carries the narration
    ViewJob v;
    v.id = "v1";
    v.subject = "eye level shot, The Hall";
    v.narration = "Elara leans close to Aldric by the fire.";
    v.width = 768;
    v.height = 432;
    ArtSubject vs{"views", "v1", v.subject, true, true, "", "", true};
    ASSERT_TRUE(svc.BuildRequest(vs, "", &v, r, err)) << err;
    EXPECT_NE(r.prompt.find("leans close"), std::string::npos);
    EXPECT_LT(r.prompt.find("leans close"), r.prompt.find("eye level shot"));   // the narration leads the camera words
    EXPECT_NE(r.prompt.find("every person shown is an adult"), std::string::npos);
    v.narration = "A child watches from the doorway.";
    EXPECT_FALSE(svc.BuildRequest(vs, "", &v, r, err));

    // the checkpoint switch, also by the API
    EXPECT_EQ(svc.Status()["checkpoint"], "grown");
    EXPECT_EQ(svc.Status()["normalCheckpoint"], "sd15");
    EXPECT_TRUE(svc.Status()["mature"].get<bool>());
    ASSERT_TRUE(svc.SetCheckpoint("other", false, err));
    EXPECT_EQ(svc.Status()["checkpoint"], "other");
    EXPECT_EQ(svc.Status()["normalCheckpoint"], "sd15");
}

TEST(MatureImages, RecentNarration) {
    EXPECT_EQ(RecentNarration(Json::object()), "");
    Json view{{"events", Json::array({Json{{"type", "narration"}, {"text", "Old words."}, {"flavour", ""}},
                                      Json{{"type", "attack"}, {"text", "Thorin hits"}, {"flavour", "Steel rings (loudly) in the \"hall\".\n  Sparks fly."}},
                                      Json{{"type", "move"}, {"text", "Finn moves"}, {"flavour", ""}}})}};
    EXPECT_EQ(RecentNarration(view), "Steel rings loudly in the hall. Sparks fly. Old words.");   // newest first
    EXPECT_EQ(RecentNarration(view, 40), "Steel rings loudly in the hall.");
    Json plain{{"events", Json::array({Json{{"type", "scene"}, {"text", "The tavern is warm."}}, Json{{"type", "move"}, {"text", "Finn moves"}}})}};
    EXPECT_EQ(RecentNarration(plain), "The tavern is warm.");
}

TEST(MatureImages, TheBundledPartyIsDrawable) {
    ImageConfig cfg;
    cfg.mature = true;
    ArtService svc(cfg, "", std::make_shared<MockImageClient>(FakePng));
    ImageRequest r;
    std::string err;
    int refused = 0;
    for (const auto& s : ArtSubjects()) {
        const bool ok = svc.BuildRequest(s, "", nullptr, r, err);
        if (s.kind == "characters") EXPECT_TRUE(ok) << s.id << ": " << err;
        if (!ok) { ++refused; std::cout << "  mature mode refuses " << s.kind << "/" << s.id << "\n"; }
    }
    std::cout << "  " << refused << " picture(s) refused in mature mode\n";
}
