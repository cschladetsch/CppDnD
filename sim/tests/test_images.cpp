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
