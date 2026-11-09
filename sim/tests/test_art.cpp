// The art service's picture-from-a-camera jobs, and the AUTOMATIC1111 client against a fake server.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "dnd/Images.hpp"
#include "dnd/Json.hpp"
#include "httplib.h"

using namespace dnd;
namespace fs = std::filesystem;

namespace {

const std::string kPng = std::string("\x89PNG\r\n\x1a\n", 8) + "fakepixels";

fs::path TempDir(const char* name) {
    const fs::path p = fs::temp_directory_path() / name;
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

std::string B64(const std::string& s) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    int val = 0, bits = -6;
    for (unsigned char c : s) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) { o.push_back(t[(val >> bits) & 63]); bits -= 6; }
    }
    if (bits > -6) o.push_back(t[((val << 8) >> (bits + 8)) & 63]);
    while (o.size() % 4) o.push_back('=');
    return o;
}

// A stand-in for AUTOMATIC1111 on a free local port.
class FakeA1111 {
public:
    FakeA1111() {
        svr.Post("/sdapi/v1/txt2img", [this](const httplib::Request& req, httplib::Response& res) { Handle("txt2img", req, res); });
        svr.Post("/sdapi/v1/img2img", [this](const httplib::Request& req, httplib::Response& res) { Handle("img2img", req, res); });
        svr.Get("/sdapi/v1/sd-models", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(R"([{"title":"sd15.safetensors [abc]","model_name":"sd15"}])", "application/json");
        });
        port = svr.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { svr.listen_after_bind(); });
        svr.wait_until_ready();
    }
    ~FakeA1111() { svr.stop(); th.join(); }
    std::string Url() const { return "http://127.0.0.1:" + std::to_string(port); }
    Json LastBody() { std::lock_guard<std::mutex> lk(m); return last; }
    std::string LastRoute() { std::lock_guard<std::mutex> lk(m); return route; }

    int status = 200;
    std::string reply = R"({"images":[")" + B64(kPng) + R"("]})";

private:
    void Handle(const char* r, const httplib::Request& req, httplib::Response& res) {
        {
            std::lock_guard<std::mutex> lk(m);
            route = r;
            last = Json::parse(req.body, nullptr, false);
        }
        res.status = status;
        res.set_content(reply, "application/json");
    }
    httplib::Server svr;
    std::thread th;
    int port = 0;
    std::mutex m;
    Json last;
    std::string route;
};

ImageConfig ConfigFor(const std::string& url) {
    ImageConfig c;
    c.backend = "a1111";
    c.url = url;
    c.timeoutSec = 20;
    return c;
}

}  // namespace

// ---------------------------------------------------------------- the A1111 client

TEST(A1111Client, DrawsFromNothingWithTxt2img) {
    FakeA1111 fake;
    auto client = MakeImageClient(ConfigFor(fake.Url()));
    ImageRequest r;
    r.prompt = "a zombie";
    r.negative = "blurry";
    r.seed = 42;
    r.steps = 11;
    r.width = 320;
    r.height = 200;
    const ImageResult out = client->Generate(r);
    ASSERT_TRUE(out.ok) << out.error;
    EXPECT_EQ(out.bytes, kPng);
    EXPECT_EQ(fake.LastRoute(), "txt2img");
    const Json b = fake.LastBody();
    EXPECT_EQ(b["prompt"], "a zombie");
    EXPECT_EQ(b["negative_prompt"], "blurry");
    EXPECT_EQ(b["seed"], 42);
    EXPECT_EQ(b["steps"], 11);
    EXPECT_EQ(b["width"], 320);
    EXPECT_EQ(b["height"], 200);
    EXPECT_FALSE(b.contains("init_images"));
}

TEST(A1111Client, StartsFromAPictureWithImg2img) {
    FakeA1111 fake;
    auto client = MakeImageClient(ConfigFor(fake.Url()));
    ImageRequest r;
    r.prompt = "a hall";
    r.initB64 = B64(kPng);
    r.denoise = 0.55;
    ASSERT_TRUE(client->Generate(r).ok);
    EXPECT_EQ(fake.LastRoute(), "img2img");
    const Json b = fake.LastBody();
    ASSERT_TRUE(b.contains("init_images"));
    EXPECT_EQ(b["init_images"][0], B64(kPng));
    EXPECT_NEAR(b["denoising_strength"].get<double>(), 0.55, 1e-9);
}

TEST(A1111Client, AServerErrorIsRefusalNotUnreachable) {
    FakeA1111 fake;
    fake.status = 500;
    fake.reply = R"({"error":"boom"})";
    const ImageResult out = MakeImageClient(ConfigFor(fake.Url()))->Generate(ImageRequest{});
    EXPECT_FALSE(out.ok);
    EXPECT_FALSE(out.unreachable);
    EXPECT_FALSE(out.error.empty());
}

TEST(A1111Client, AReplyWithoutAPictureIsAnError) {
    FakeA1111 fake;
    fake.reply = R"({"images":[]})";
    const ImageResult out = MakeImageClient(ConfigFor(fake.Url()))->Generate(ImageRequest{});
    EXPECT_FALSE(out.ok);
    EXPECT_FALSE(out.error.empty());
}

TEST(A1111Client, NothingListeningFailsCleanly) {
    int port = 0;
    {
        httplib::Server s;
        port = s.bind_to_any_port("127.0.0.1");
    }  // closed again: nobody is listening there
    ImageConfig c = ConfigFor("http://127.0.0.1:" + std::to_string(port));
    c.timeoutSec = 2;
    auto client = MakeImageClient(c);
    const ImageResult out = client->Generate(ImageRequest{});
    EXPECT_FALSE(out.ok);
    EXPECT_FALSE(out.error.empty());  // whether this is flagged unreachable depends on how the OS refuses (refused vs reset)
    EXPECT_FALSE(client->Probe()["reachable"].get<bool>());
}

TEST(A1111Client, ProbeListsTheServersModels) {
    FakeA1111 fake;
    const Json p = MakeImageClient(ConfigFor(fake.Url()))->Probe();
    EXPECT_TRUE(p["reachable"].get<bool>());
    EXPECT_FALSE(p["models"].empty());
}

// ---------------------------------------------------------------- camera pictures through the service

TEST(ArtViews, DrawnPictureLandsInViewsAndReportsDone) {
    const fs::path root = TempDir("dnd_artviews_done");
    std::mutex m;
    std::vector<ImageRequest> seen;
    auto client = std::make_shared<MockImageClient>([&](const ImageRequest& r) {
        std::lock_guard<std::mutex> lk(m);
        seen.push_back(r);
        ImageResult o;
        o.ok = true;
        o.bytes = kPng;
        return o;
    });
    ArtService svc(ImageConfig{}, (root / "art").string(), client);
    EXPECT_EQ(svc.View("v1")["state"], "unknown");

    ViewJob j;
    j.id = "v1";
    j.subject = "eye level shot, a hall";
    j.initB64 = B64(kPng);
    j.strength = 0.6;
    std::string err;
    const Json q = svc.EnqueueView(j, err);
    ASSERT_TRUE(err.empty()) << err;
    EXPECT_EQ(q["id"], "v1");
    svc.WaitIdle();

    const Json v = svc.View("v1");
    EXPECT_EQ(v["state"], "done");
    EXPECT_TRUE(fs::exists(root / "art" / "views" / "v1.png"));
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_NE(seen[0].prompt.find("a hall"), std::string::npos);
    EXPECT_EQ(seen[0].initB64, B64(kPng));
    EXPECT_NEAR(seen[0].denoise, 0.6, 1e-9);
    EXPECT_EQ(seen[0].width, 768);
    EXPECT_EQ(seen[0].height, 432);
}

TEST(ArtViews, TheSameIdAndSubjectGetTheSameSeed) {
    const fs::path root = TempDir("dnd_artviews_seed");
    std::mutex m;
    std::vector<std::uint32_t> seeds;
    auto client = std::make_shared<MockImageClient>([&](const ImageRequest& r) {
        std::lock_guard<std::mutex> lk(m);
        seeds.push_back(r.seed);
        ImageResult o;
        o.ok = true;
        o.bytes = kPng;
        return o;
    });
    ArtService svc(ImageConfig{}, (root / "art").string(), client);
    std::string err;
    for (const char* id : {"same", "same", "other"}) {
        ViewJob j;
        j.id = id;
        j.subject = "x";
        svc.EnqueueView(j, err);
        svc.WaitIdle();
    }
    ASSERT_EQ(seeds.size(), 3u);
    EXPECT_EQ(seeds[0], seeds[1]);
    EXPECT_NE(seeds[0], seeds[2]);
}

TEST(ArtViews, AFailedDrawIsReported) {
    const fs::path root = TempDir("dnd_artviews_fail");
    auto client = std::make_shared<MockImageClient>([](const ImageRequest&) {
        ImageResult o;
        o.error = "out of memory";
        return o;
    });
    ArtService svc(ImageConfig{}, (root / "art").string(), client);
    ViewJob j;
    j.id = "bad";
    j.subject = "x";
    std::string err;
    svc.EnqueueView(j, err);
    svc.WaitIdle();
    const Json v = svc.View("bad");
    EXPECT_EQ(v["state"], "failed");
    EXPECT_NE(v["error"].get<std::string>().find("out of memory"), std::string::npos);
    EXPECT_FALSE(fs::exists(root / "art" / "views" / "bad.png"));
}

TEST(ArtViews, UnreachableServerDropsTheRestOfTheQueue) {
    const fs::path root = TempDir("dnd_artviews_unreachable");
    std::atomic<int> calls{0};
    auto client = std::make_shared<MockImageClient>([&](const ImageRequest&) {
        ++calls;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        ImageResult o;
        o.error = "refused";
        o.unreachable = true;
        return o;
    });
    ArtService svc(ImageConfig{}, (root / "art").string(), client);
    std::string err;
    for (const char* id : {"a", "b", "c"}) {
        ViewJob j;
        j.id = id;
        j.subject = "x";
        svc.EnqueueView(j, err);
    }
    svc.WaitIdle();
    EXPECT_EQ(calls.load(), 1);
    EXPECT_EQ(svc.View("a")["state"], "failed");
    EXPECT_NE(svc.View("a")["error"].get<std::string>().find("unreachable"), std::string::npos);
}

TEST(ArtViews, BadIdsAreRefused) {
    const fs::path root = TempDir("dnd_artviews_ids");
    ArtService svc(ImageConfig{}, (root / "art").string(), std::make_shared<MockImageClient>([](const ImageRequest&) { return ImageResult{}; }));
    for (const char* id : {"", "../escape", "a/b", "with space"}) {
        ViewJob j;
        j.id = id;
        j.subject = "x";
        std::string err;
        svc.EnqueueView(j, err);
        EXPECT_FALSE(err.empty()) << "id: '" << id << "'";
    }
    EXPECT_FALSE(fs::exists(root / "escape.png"));
}
