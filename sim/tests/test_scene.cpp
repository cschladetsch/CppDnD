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


TEST(Scene, CameraAndViews) {
    namespace fs = std::filesystem;
    // ---- an exploring scene: open field, party at the south end, people of the place at the north
    const Json view = Json::parse(R"({"mode":"exploring","hero":"Thorin","combat":null,
        "party":[{"id":"thorin","name":"Thorin","class":"fighter","dead":false},{"id":"elara","name":"Elara","class":"wizard","dead":false}],
        "location":{"name":"The Mist Road","description":"A rutted road. Wolves howl.","kind":"wilderness","npcs":[{"id":"marta","name":"Marta Hollin","type":"innkeeper"}]}})");
    const Json scene = BuildScene(view);
    EXPECT_TRUE(scene["props"].size() == 3 && scene["grid"]["width"] == 16 && scene["hero"]["id"] == "thorin");
    const Camera cam = Camera::FromJson(scene["camera"]);
    EXPECT_TRUE(std::fabs(cam.x - 7.5) < 0.01 && std::fabs(cam.y - 9.5) < 0.01 && cam.yaw > 5 && cam.yaw < 12);  // eyes of Thorin, turned to Marta
    const std::string seen = DescribeView(scene, cam);
    EXPECT_TRUE(seen.find("eye level shot") != std::string::npos && seen.find("The Mist Road") != std::string::npos);
    EXPECT_NE(seen.find("Marta Hollin, an innkeeper in the middle distance"), std::string::npos);
    EXPECT_EQ(seen.find("Wolves"), std::string::npos);  // only the first sentence of the place
    Camera back = cam;
    back.yaw += 180;
    back.Clamp();
    EXPECT_NE(DescribeView(scene, back).find("nobody in view"), std::string::npos);  // Elara is beside, Marta behind
    Camera top = cam;
    top.pitch = -80;
    top.z = 12;
    EXPECT_NE(DescribeView(scene, top).find("top-down view"), std::string::npos);
    Camera wide = cam, tele = cam;
    wide.fov = 110;
    tele.fov = 25;
    EXPECT_TRUE(CameraWords(wide).find("ultra wide") != std::string::npos && CameraWords(tele).find("telephoto") != std::string::npos);

    // ---- the camera: clamping and projection
    const Camera c = Camera::FromJson(Json{{"pitch", 1000}, {"fov", 5}, {"yaw", -90}, {"x", "bad"}});
    EXPECT_TRUE(c.pitch == 89 && c.fov == 15 && c.yaw == 270 && c.x == 0);
    Camera o;
    o.x = 0;
    o.y = 0;
    o.z = 1;
    o.yaw = 0;  // north: toward smaller y
    o.pitch = 0;
    double sx = 9, sy = 9, depth = 0;
    EXPECT_TRUE(Project(o, 16.0 / 9.0, 0, -5, 1, sx, sy, depth) && std::fabs(sx) < 1e-9 && std::fabs(sy) < 1e-9 && std::fabs(depth - 5) < 1e-9);
    EXPECT_FALSE(Project(o, 16.0 / 9.0, 0, 5, 1, sx, sy, depth));      // behind
    EXPECT_TRUE(Project(o, 16.0 / 9.0, 2, -5, 1, sx, sy, depth) && sx > 0);   // east is to the right when facing north
    EXPECT_TRUE(Project(o, 16.0 / 9.0, 0, -5, 3, sx, sy, depth) && sy > 0);   // higher is higher in the picture
    o.yaw = 90;  // east: right is south
    EXPECT_TRUE(Project(o, 16.0 / 9.0, 5, 2, 1, sx, sy, depth) && sx > 0);

    // ---- a combat scene uses the real grid and positions
    const Json cv = Json::parse(R"({"mode":"combat","hero":"Thorin","party":[{"id":"thorin","name":"Thorin","class":"fighter"}],
        "combat":{"grid":{"width":4,"height":3,"rows":["....","..#.","...."]},
        "actors":[{"id":0,"team":"party","name":"Thorin","classId":"fighter","pos":[0,1],"life":"alive"},
                  {"id":1,"team":"monsters","name":"Zombie 2","monsterId":"zombie","pos":[3,1],"life":"alive"}]}})");
    const Json cs = BuildScene(cv);
    EXPECT_TRUE(cs["grid"]["width"] == 4 && cs["props"].size() == 2 && cs["hero"]["id"] == "thorin" && cs["props"][1]["team"] == "foe");
    const Camera cc = Camera::FromJson(cs["camera"]);
    EXPECT_TRUE(std::fabs(cc.yaw - 90) < 0.01);  // turned toward the zombie
    EXPECT_NE(DescribeView(cs, cc).find("a zombie"), std::string::npos);

    // ---- views: drawn from a camera, starting from the live preview
    const fs::path root = fs::temp_directory_path() / "dnd_views_test";
    fs::remove_all(root);
    const std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + "fakepixels";
    std::mutex mu;
    std::vector<ImageRequest> reqs;
    bool down = false;
    auto client = std::make_shared<MockImageClient>([&](const ImageRequest& r) {
        std::lock_guard<std::mutex> lk(mu);
        reqs.push_back(r);
        ImageResult out;
        if (down) { out.error = "no server"; out.unreachable = true; return out; }
        out.ok = true;
        out.bytes = png;
        return out;
    });
    ImageConfig icfg;
    icfg.style.prefix = "STYLE";
    ArtService svc(icfg, (root / "art").string(), client);
    std::string err;
    ViewJob job;
    job.id = "v1";
    job.subject = "eye level shot, a road";
    job.initB64 = "iVBORw0KGgo=";
    job.strength = 0.5;
    job.width = 770;
    job.height = 430;
    svc.EnqueueView(job, err);
    EXPECT_TRUE(err.empty());
    svc.WaitIdle();
    {
        std::lock_guard<std::mutex> lk(mu);
        EXPECT_TRUE(reqs.size() == 1 && reqs[0].prompt == "STYLE, eye level shot, a road" && reqs[0].initB64 == "iVBORw0KGgo=" && reqs[0].denoise == 0.5);
        EXPECT_TRUE(reqs[0].width == 768 && reqs[0].height == 424);  // snapped to multiples of 8
    }
    const Json done = svc.View("v1");
    EXPECT_TRUE(done["state"] == "done" && done["url"] == "/art/views/v1.png" && fs::exists(root / "art" / "views" / "v1.png"));
    EXPECT_EQ(svc.View("nope")["state"], "unknown");
    ViewJob bad;
    bad.id = "../x";
    bad.subject = "s";
    svc.EnqueueView(bad, err);
    EXPECT_FALSE(err.empty());
    err.clear();
    {
        std::lock_guard<std::mutex> lk(mu);
        down = true;
    }
    job.id = "v2";
    svc.EnqueueView(job, err);
    job.id = "v3";
    svc.EnqueueView(job, err);
    svc.WaitIdle();
    EXPECT_TRUE(svc.View("v2")["state"] == "failed" && svc.View("v2")["error"].get<std::string>().find("unreachable") == 0);
    EXPECT_TRUE(svc.View("v3")["state"] == "failed" || svc.View("v3")["state"] == "queued");  // dropped with the dead server, or tried and failed
    svc.WaitIdle();
    EXPECT_EQ(svc.View("v3")["state"], "failed");
    fs::remove_all(root);
}
