#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <mutex>

#include "dnd/Illustrator.hpp"
#include "dnd/Images.hpp"
#include "dnd/Llm.hpp"

using namespace dnd;

namespace {
Json AView(int lastSeq) {
    Json events = Json::array();
    events.push_back(Json{{"seq", 1}, {"type", "scene"}, {"text", "The Gilded Stag is warm and loud."}, {"flavour", ""}});
    events.push_back(Json{{"seq", 2}, {"type", "turn_start"}, {"text", "Thorin's turn"}, {"flavour", ""}});
    events.push_back(Json{{"seq", 3}, {"type", "narration"}, {"text", "Elara unpins her cloak and lets it fall."}, {"flavour", ""}});
    events.push_back(Json{{"seq", 4}, {"type", "attack"}, {"text", "Thorin hits the goblin for 7"}, {"flavour", "Thorin's hammer\n caves in the goblin's shield."}});
    return Json{{"lastSeq", lastSeq}, {"hero", "Thorin"}, {"events", events},
                {"location", {{"name", "The Gilded Stag"}, {"description", "A smoky inn with a low beamed ceiling."}}},
                {"party", Json::array({Json{{"name", "Thorin"}, {"gender", "male"}, {"class", "fighter"}, {"tags", {"humanoid", "dwarf"}}, {"hp", 10}, {"maxHp", 30}},
                                       Json{{"name", "Elara"}, {"gender", "female"}, {"class", "wizard"}, {"tags", {"humanoid", "elf"}}, {"hp", 12}, {"maxHp", 12}}})}};
}
}  // namespace

TEST(Illustrator, GathersTheStoryNotTheBookkeeping) {
    const StoryMaterial m = GatherStory(AView(4), "eye level shot", false);
    ASSERT_EQ(m.lines.size(), 3u);  // the turn start is left out
    EXPECT_EQ(m.lines[2].text, "Thorin's hammer caves in the goblin's shield.");  // narration over the plain text, on one line
    EXPECT_EQ(m.lastSeq, 4);
    EXPECT_NE(m.place.find("smoky inn"), std::string::npos);
    EXPECT_NE(m.party.find("Thorin (male, dwarf, fighter, badly wounded), the hero"), std::string::npos) << m.party;
    EXPECT_NE(m.party.find("Elara (female, elf, wizard)"), std::string::npos);
    EXPECT_EQ(GatherStory(AView(4), "", false, 2).lines.size(), 2u);
}

TEST(Illustrator, CondensesThenOnlyReadsWhatIsNew) {
    std::string lastUser, lastSystem;
    int calls = 0;
    auto llm = std::make_shared<MockLlmClient>([&](const std::string& sys, const std::string& user, bool json) {
        ++calls;
        lastUser = user;
        lastSystem = sys;
        EXPECT_TRUE(json);
        LlmReply r;
        r.ok = true;
        r.content = "Sure!\n```json\n{\"story\": \"A dwarf and an elf woman fight a goblin in a smoky inn.\", \"prompt\": \"a stocky bearded dwarf (swinging) a hammer, [an elf woman] in a green dress, smoky inn, candlelight\"}\n```";
        return r;
    });
    Illustrator ill(llm, Illustrator::Config{120, 20});
    StoryDigest d;
    StoryMaterial m = GatherStory(AView(4), "eye level shot, The Gilded Stag", false);
    auto shot = ill.Compose(d, m);
    ASSERT_TRUE(shot.ok) << shot.error;
    EXPECT_EQ(shot.prompt, "a stocky bearded dwarf swinging a hammer, an elf woman in a green dress, smoky inn, candlelight");  // no brackets or weights
    EXPECT_EQ(d.text, "A dwarf and an elf woman fight a goblin in a smoky inn.");
    EXPECT_EQ(d.lastSeq, 4);
    EXPECT_EQ(d.updates, 1);
    EXPECT_NE(lastUser.find("(nothing yet)"), std::string::npos);
    EXPECT_NE(lastUser.find("unpins her cloak"), std::string::npos);
    EXPECT_NE(lastUser.find("THE CAMERA: eye level shot"), std::string::npos);
    EXPECT_NE(lastSystem.find("at most 120 words"), std::string::npos);
    EXPECT_EQ(lastSystem.find("adult story"), std::string::npos);

    // the next picture sends the condensed story and only the events after it
    Json v = AView(5);
    v["events"].push_back(Json{{"seq", 5}, {"type", "narration"}, {"text", "The goblin flees into the rain."}, {"flavour", ""}});
    shot = ill.Compose(d, GatherStory(v, "", true));
    ASSERT_TRUE(shot.ok);
    EXPECT_NE(lastUser.find("A dwarf and an elf woman fight"), std::string::npos);
    EXPECT_NE(lastUser.find("flees into the rain"), std::string::npos);
    EXPECT_EQ(lastUser.find("unpins her cloak"), std::string::npos);
    EXPECT_NE(lastSystem.find("adult story"), std::string::npos);  // mature wording
    EXPECT_EQ(d.lastSeq, 5);

    // rewound (the game's newest event is older than the digest): rebuilt from what the game still has
    shot = ill.Compose(d, GatherStory(AView(3), "", false));
    ASSERT_TRUE(shot.ok);
    EXPECT_NE(lastUser.find("(nothing yet)"), std::string::npos);
    EXPECT_NE(lastUser.find("unpins her cloak"), std::string::npos);
    EXPECT_EQ(d.lastSeq, 4);

    // a failing model leaves the digest alone
    const StoryDigest before = d;
    auto bad = std::make_shared<MockLlmClient>([](const std::string&, const std::string&, bool) { LlmReply r; r.ok = true; r.content = "I can't draw that."; return r; });
    Illustrator ill2(bad, {});
    shot = ill2.Compose(d, GatherStory(v, "", false));
    EXPECT_FALSE(shot.ok);
    EXPECT_FALSE(shot.error.empty());
    EXPECT_EQ(d.text, before.text);
    EXPECT_EQ(d.lastSeq, before.lastSeq);
}

TEST(Illustrator, StoryStateRoundTripsOnDisk) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "dnd_story_state";
    fs::remove_all(dir);
    {
        StoryState s;
        s.dir = dir.string();
        s.LoadIfNeeded();
        EXPECT_TRUE(s.digest.text.empty());
        s.digest.text = "Once upon a time.";
        s.digest.lastSeq = 9;
        s.Save();
    }
    StoryState t;
    t.dir = dir.string();
    t.LoadIfNeeded();
    EXPECT_EQ(t.digest.text, "Once upon a time.");
    EXPECT_EQ(t.digest.lastSeq, 9);
    fs::remove_all(dir);
}

TEST(Illustrator, ViewsFromTheStoryAreWordedAsPhotographs) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "dnd_story_views";
    fs::remove_all(root);
    std::mutex mu;
    std::vector<ImageRequest> seen;
    auto client = std::make_shared<MockImageClient>([&](const ImageRequest& r) {
        std::lock_guard<std::mutex> lk(mu);
        seen.push_back(r);
        ImageResult o; o.ok = true; o.bytes = std::string("\x89PNG\r\n\x1a\n", 8) + "px"; return o;
    });
    ImageConfig cfg;
    ArtService svc(cfg, root.string(), client);
    std::string err;

    ViewJob v;
    v.id = "s1";
    v.subject = "eye level shot, The Hall";
    v.module = "hollow_crown";  // "dark medieval fantasy illustration, painterly, ..." and "photo" in its negative
    v.photo = true;
    v.compose = [](std::string&) { return std::string("a grey-bearded king on a throne, torchlight"); };
    svc.EnqueueView(v, err);
    svc.WaitIdle();
    ASSERT_EQ(seen.size(), 1u) << err;
    const ImageRequest r = seen[0];
    EXPECT_EQ(r.prompt.rfind(cfg.story.prefix, 0), 0u) << r.prompt;
    EXPECT_NE(r.prompt.find("a grey-bearded king on a throne"), std::string::npos);
    EXPECT_EQ(r.prompt.find("eye level shot, The Hall"), std::string::npos);  // the story's words replace the camera's
    EXPECT_NE(r.prompt.find("dark medieval fantasy"), std::string::npos);    // the module's setting stays...
    EXPECT_EQ(r.prompt.find("illustration"), std::string::npos);             // ...without its painting words
    EXPECT_EQ(r.prompt.find("painterly"), std::string::npos);
    EXPECT_EQ((", " + r.negative + ",").find(", photo,"), std::string::npos) << r.negative;
    EXPECT_NE(r.negative.find("3d render"), std::string::npos);
    EXPECT_EQ(r.steps, cfg.story.steps);
    EXPECT_EQ(r.sampler, cfg.story.sampler);
    EXPECT_EQ(svc.View("s1")["prompt"], "a grey-bearded king on a throne, torchlight");
    EXPECT_TRUE(svc.View("s1")["fromStory"].get<bool>());

    // the illustrator failing: the camera words draw, and the view says why
    seen.clear();
    ViewJob w = v;
    w.id = "s2";
    w.compose = [](std::string& why) { why = "model down"; return std::string(); };
    svc.EnqueueView(w, err);
    svc.WaitIdle();
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_NE(seen[0].prompt.find("eye level shot, The Hall"), std::string::npos);
    EXPECT_EQ(svc.View("s2")["storyError"], "model down");
    fs::remove_all(root);
}

TEST(Illustrator, MatureStoryWordsAreStillGuarded) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "dnd_story_guard";
    fs::remove_all(root);
    std::vector<ImageRequest> seen;
    std::mutex mu;
    auto client = std::make_shared<MockImageClient>([&](const ImageRequest& r) {
        std::lock_guard<std::mutex> lk(mu);
        seen.push_back(r);
        ImageResult o; o.ok = true; o.bytes = std::string("\x89PNG\r\n\x1a\n", 8) + "px"; return o;
    });
    ImageConfig cfg;
    cfg.mature = true;
    ArtService svc(cfg, root.string(), client);
    std::string err;
    ViewJob v;
    v.id = "m1";
    v.subject = "eye level shot, The Hall";
    v.photo = true;
    v.compose = [](std::string&) { return std::string("a girl by the fire"); };
    svc.EnqueueView(v, err);
    svc.WaitIdle();
    ASSERT_EQ(seen.size(), 1u);  // the illustrator's words are dropped, the camera's are drawn
    EXPECT_EQ(seen[0].prompt.find("girl"), std::string::npos);
    EXPECT_NE(seen[0].prompt.find("every person shown is an adult"), std::string::npos);
    EXPECT_EQ(seen[0].negative.substr(seen[0].negative.size() - AdultsOnlyNegative().size()), AdultsOnlyNegative());
    EXPECT_NE(svc.View("m1").value("storyError", "").find("adults"), std::string::npos);
    EXPECT_FALSE(svc.View("m1")["fromStory"].get<bool>());
    fs::remove_all(root);
}

TEST(Illustrator, StoryBlockLoads) {
    namespace fs = std::filesystem;
    const fs::path f = fs::temp_directory_path() / "dnd_story_images.json";
    { std::ofstream o(f); o << R"({ // comments are fine
        "story": {"model": "qwen2.5:7b", "url": "http://127.0.0.1:11434", "photo": false, "promptWords": 40, "steps": 22}})"; }
    std::string err;
    auto c = ImageConfig::Load(f.string(), "", "", err);
    ASSERT_TRUE(c) << err;
    EXPECT_TRUE(c->story.enabled);
    EXPECT_EQ(c->story.model, "qwen2.5:7b");
    EXPECT_FALSE(c->story.photo);
    EXPECT_EQ(c->story.promptWords, 40);
    EXPECT_EQ(c->story.steps, 22);
    { std::ofstream o(f); o << R"({"story": true})"; }
    EXPECT_FALSE(ImageConfig::Load(f.string(), "", "", err));
    fs::remove(f);
}
