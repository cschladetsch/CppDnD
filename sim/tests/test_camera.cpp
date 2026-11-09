// The camera model: clamping, projection, the scene a camera sees, and the words it adds to a picture prompt.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <limits>
#include <numbers>
#include <regex>
#include <string>
#include <vector>

#include "dnd/Json.hpp"
#include "dnd/Scene.hpp"

using namespace dnd;

namespace {

constexpr double kAspect = 16.0 / 9.0;

Camera At(double x, double y, double yaw = 0, double pitch = 0, double fov = 70, double z = 1.06) {
    Camera c;
    c.x = x;
    c.y = y;
    c.z = z;
    c.yaw = yaw;
    c.pitch = pitch;
    c.fov = fov;
    return c;
}

Json Prop(const char* name, const char* team, const char* kind, double x, double y, const char* life = "alive", bool hero = false) {
    return Json{{"id", name}, {"name", name}, {"team", team}, {"kind", kind}, {"x", x}, {"y", y}, {"life", life}, {"hero", hero}};
}

Json SceneOf(const std::vector<Json>& props, const char* place = "The Hall", const char* desc = "A cold hall.") {
    Json p = Json::array();
    for (const auto& x : props) p.push_back(x);
    return Json{{"props", p}, {"location", Json{{"name", place}, {"description", desc}}}, {"grid", Json{{"width", 16}, {"height", 12}, {"rows", Json::array()}}}};
}

bool Has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

int CountMatches(const std::string& s, const char* pattern) {
    const std::regex re(pattern);
    return static_cast<int>(std::distance(std::sregex_iterator(s.begin(), s.end(), re), std::sregex_iterator()));
}

}  // namespace

// ---------------------------------------------------------------- Camera

TEST(Camera, RoundTripsThroughJson) {
    const Camera c = At(3.25, -4.5, 123.45, -17.5, 55, 2.5);
    const Camera d = Camera::FromJson(c.ToJson());
    EXPECT_NEAR(d.x, 3.25, 0.01);
    EXPECT_NEAR(d.y, -4.5, 0.01);
    EXPECT_NEAR(d.z, 2.5, 0.01);
    EXPECT_NEAR(d.yaw, 123.45, 0.01);
    EXPECT_NEAR(d.pitch, -17.5, 0.01);
    EXPECT_NEAR(d.fov, 55, 0.01);
}

TEST(Camera, MissingAndBadFieldsKeepTheFallback) {
    const Camera base = At(7, 8, 90, 5, 60, 2);
    const Camera c = Camera::FromJson(Json{{"x", "far"}, {"yaw", nullptr}, {"pitch", 12}}, base);
    EXPECT_EQ(c.x, 7);
    EXPECT_EQ(c.y, 8);
    EXPECT_EQ(c.yaw, 90);
    EXPECT_EQ(c.pitch, 12);
    EXPECT_EQ(c.fov, 60);
    const Camera d = Camera::FromJson(Json("not an object"), base);
    EXPECT_EQ(d.x, 7);
    EXPECT_EQ(d.fov, 60);
}

TEST(Camera, ClampsEveryField) {
    Camera c = At(5000, -5000, 10, 500, 500, 500);
    c.Clamp();
    EXPECT_EQ(c.x, 800);
    EXPECT_EQ(c.y, -400);
    EXPECT_EQ(c.z, 60);
    EXPECT_EQ(c.pitch, 89);
    EXPECT_EQ(c.fov, 120);
    Camera low = At(0, 0, 0, -500, 1, -5);
    low.Clamp();
    EXPECT_EQ(low.pitch, -89);
    EXPECT_EQ(low.fov, 15);
    EXPECT_EQ(low.z, 0.05);
}

TEST(Camera, NotANumberFallsBackToSafeValues) {
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    Camera c = At(nan, inf, nan, nan, nan, nan);
    c.Clamp();
    EXPECT_TRUE(std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z) && std::isfinite(c.yaw) && std::isfinite(c.pitch) && std::isfinite(c.fov));
    EXPECT_EQ(c.fov, 70);
    EXPECT_EQ(c.yaw, 0);
}

struct YawCase {
    double in, out;
};
class CameraYaw : public ::testing::TestWithParam<YawCase> {};
TEST_P(CameraYaw, WrapsIntoZeroTo360) {
    Camera c = At(0, 0, GetParam().in);
    c.Clamp();
    EXPECT_NEAR(c.yaw, GetParam().out, 1e-9);
}
INSTANTIATE_TEST_SUITE_P(Wraps, CameraYaw, ::testing::Values(YawCase{-90, 270}, YawCase{0, 0}, YawCase{360, 0}, YawCase{450, 90}, YawCase{-720, 0}, YawCase{359.5, 359.5}));

// ---------------------------------------------------------------- Projection

struct Facing {
    const char* name;
    double yaw, ax, ay, rx, ry;  // heading, the unit step ahead, the unit step to the right (x east, y south)
};
class Projection : public ::testing::TestWithParam<Facing> {};

TEST_P(Projection, AheadIsTheCenterOfThePicture) {
    const Facing f = GetParam();
    const Camera cam = At(10, 10, f.yaw, 0, 70, 1);
    double sx = 9, sy = 9, depth = 0;
    ASSERT_TRUE(Project(cam, kAspect, 10 + 5 * f.ax, 10 + 5 * f.ay, 1, sx, sy, depth));
    EXPECT_NEAR(sx, 0, 1e-9);
    EXPECT_NEAR(sy, 0, 1e-9);
    EXPECT_NEAR(depth, 5, 1e-9);
}

TEST_P(Projection, TheRightHandSideIsOnTheRight) {
    const Facing f = GetParam();
    const Camera cam = At(10, 10, f.yaw, 0, 70, 1);
    double sx = 0, sy = 0, depth = 0;
    ASSERT_TRUE(Project(cam, kAspect, 10 + 5 * f.ax + f.rx, 10 + 5 * f.ay + f.ry, 1, sx, sy, depth));
    EXPECT_NEAR(sx, 1.0 / (5.0 * std::tan(35.0 * std::numbers::pi / 180.0) * kAspect), 1e-9);
    ASSERT_TRUE(Project(cam, kAspect, 10 + 5 * f.ax - f.rx, 10 + 5 * f.ay - f.ry, 1, sx, sy, depth));
    EXPECT_LT(sx, 0);
}

TEST_P(Projection, BehindTheCameraIsNotVisible) {
    const Facing f = GetParam();
    const Camera cam = At(10, 10, f.yaw, 0, 70, 1);
    double sx, sy, depth;
    EXPECT_FALSE(Project(cam, kAspect, 10 - 5 * f.ax, 10 - 5 * f.ay, 1, sx, sy, depth));
}

INSTANTIATE_TEST_SUITE_P(Headings, Projection,
                         ::testing::Values(Facing{"north", 0, 0, -1, 1, 0}, Facing{"east", 90, 1, 0, 0, 1}, Facing{"south", 180, 0, 1, -1, 0}, Facing{"west", 270, -1, 0, 0, -1}),
                         [](const ::testing::TestParamInfo<Facing>& i) { return std::string(i.param.name); });

TEST(ProjectionPitch, LookingUpPutsLevelThingsBelowCenter) {
    double sx, sy, depth;
    ASSERT_TRUE(Project(At(0, 0, 0, 30, 70, 1), kAspect, 0, -5, 1, sx, sy, depth));
    EXPECT_LT(sy, 0);
    ASSERT_TRUE(Project(At(0, 0, 0, -30, 70, 1), kAspect, 0, -5, 1, sx, sy, depth));
    EXPECT_GT(sy, 0);
}

TEST(ProjectionPitch, HigherIsHigherInThePicture) {
    double sx, low, high, depth;
    ASSERT_TRUE(Project(At(0, 0, 0, 0, 70, 1), kAspect, 0, -5, 0, sx, low, depth));
    ASSERT_TRUE(Project(At(0, 0, 0, 0, 70, 1), kAspect, 0, -5, 3, sx, high, depth));
    EXPECT_LT(low, high);
}

TEST(ProjectionLens, NarrowerFieldOfViewMagnifies) {
    double wide, narrow, sy, depth;
    ASSERT_TRUE(Project(At(0, 0, 0, 0, 90, 1), kAspect, 1, -5, 1, wide, sy, depth));
    ASSERT_TRUE(Project(At(0, 0, 0, 0, 30, 1), kAspect, 1, -5, 1, narrow, sy, depth));
    EXPECT_GT(narrow, wide * 2);
}

TEST(ProjectionLens, AspectScalesTheHorizontalAxisOnly) {
    double sx1, sy1, sx2, sy2, depth;
    ASSERT_TRUE(Project(At(0, 0, 0, 0, 70, 1), 1.0, 1, -5, 2, sx1, sy1, depth));
    ASSERT_TRUE(Project(At(0, 0, 0, 0, 70, 1), kAspect, 1, -5, 2, sx2, sy2, depth));
    EXPECT_NEAR(sx1 / sx2, kAspect, 1e-9);
    EXPECT_NEAR(sy1, sy2, 1e-12);
}

TEST(ProjectionLens, ADegenerateAspectDoesNotDivideByZero) {
    double sx, sy, depth;
    ASSERT_TRUE(Project(At(0, 0, 0, 0, 70, 1), 0.0, 1, -5, 1, sx, sy, depth));
    EXPECT_TRUE(std::isfinite(sx));
}

// ---------------------------------------------------------------- Camera words

struct WordsCase {
    double pitch, fov, z;
    const char* expect;
};
class CameraWordsTable : public ::testing::TestWithParam<WordsCase> {};
TEST_P(CameraWordsTable, Mentions) {
    const auto& p = GetParam();
    EXPECT_TRUE(Has(CameraWords(At(0, 0, 0, p.pitch, p.fov, p.z)), p.expect)) << CameraWords(At(0, 0, 0, p.pitch, p.fov, p.z));
}
INSTANTIATE_TEST_SUITE_P(Angles, CameraWordsTable,
                         ::testing::Values(WordsCase{-85, 70, 1, "top-down"}, WordsCase{-60, 70, 1, "top-down"}, WordsCase{-40, 70, 1, "high angle shot"}, WordsCase{-15, 70, 1, "slightly high angle"},
                                           WordsCase{0, 70, 1, "eye level"}, WordsCase{8, 70, 1, "eye level"}, WordsCase{15, 70, 1, "slightly low angle"}, WordsCase{35, 70, 1, "low angle shot"},
                                           WordsCase{70, 70, 1, "worm's eye"}, WordsCase{0, 20, 1, "telephoto"}, WordsCase{0, 45, 1, "portrait lens"}, WordsCase{0, 90, 1, "wide angle lens"},
                                           WordsCase{0, 110, 1, "ultra wide"}, WordsCase{-30, 70, 5, "from high above"}));

TEST(CameraWords, ANaturalLensAddsNoLensWords) {
    const std::string w = CameraWords(At(0, 0, 0, 0, 70, 1));
    EXPECT_FALSE(Has(w, "lens"));
    EXPECT_FALSE(Has(w, "above"));
}

// ---------------------------------------------------------------- BuildScene

namespace {
Json Exploring(const char* hero, std::vector<Json> party, std::vector<Json> npcs = {}) {
    Json p = Json::array(), n = Json::array();
    for (auto& x : party) p.push_back(x);
    for (auto& x : npcs) n.push_back(x);
    return Json{{"mode", "exploring"}, {"hero", hero}, {"combat", nullptr}, {"party", p},
                {"location", Json{{"name", "The Mist Road"}, {"description", "A rutted road."}, {"kind", "wilderness"}, {"npcs", n}}}};
}
Json Member(const char* id, const char* name, const char* cls, bool dead = false) { return Json{{"id", id}, {"name", name}, {"class", cls}, {"dead", dead}}; }
Json Npc(const char* id, const char* name, const char* type) { return Json{{"id", id}, {"name", name}, {"type", type}}; }
}  // namespace

TEST(SceneLayout, TheDeadAreNotPlaced) {
    const Json s = BuildScene(Exploring("Thorin", {Member("thorin", "Thorin", "fighter"), Member("elara", "Elara", "wizard", true)}));
    ASSERT_EQ(s["props"].size(), 1u);
    EXPECT_EQ(s["props"][0]["name"], "Thorin");
}

TEST(SceneLayout, WithNobodyThereTheCameraSitsInTheMiddleOfTheField) {
    const Json s = BuildScene(Exploring("Nobody", {}));
    EXPECT_TRUE(s["hero"].is_null());
    EXPECT_EQ(s["props"].size(), 0u);
    const Camera c = Camera::FromJson(s["camera"]);
    EXPECT_NEAR(c.x, 8, 0.01);
    EXPECT_NEAR(c.y, 6, 0.01);
}

struct HeroCase {
    const char* hero;
    const char* expectId;
};
class HeroLookup : public ::testing::TestWithParam<HeroCase> {};
TEST_P(HeroLookup, FindsTheHeroByNameOrIdIgnoringCase) {
    const Json s = BuildScene(Exploring(GetParam().hero, {Member("thorin", "Thorin", "fighter"), Member("elara", "Elara", "wizard")}));
    ASSERT_FALSE(s["hero"].is_null());
    EXPECT_EQ(s["hero"]["id"], GetParam().expectId);
    int marked = 0;
    for (const auto& p : s["props"]) marked += p["hero"].get<bool>() ? 1 : 0;
    EXPECT_EQ(marked, 1);
}
INSTANTIATE_TEST_SUITE_P(Names, HeroLookup,
                         ::testing::Values(HeroCase{"Elara", "elara"}, HeroCase{"elara", "elara"}, HeroCase{"ELARA", "elara"}, HeroCase{"Thorin", "thorin"}, HeroCase{"someone else", "thorin"}, HeroCase{"", "thorin"}));

TEST(SceneLayout, PeopleOfThePlaceStandAtTheFarEndInsideTheField) {
    const Json s = BuildScene(Exploring("Thorin", {Member("thorin", "Thorin", "fighter")}, {Npc("a", "Ann", "innkeeper"), Npc("b", "Bob", "smith"), Npc("c", "Cy", "guard")}));
    std::set<int> xs;
    for (const auto& p : s["props"]) {
        EXPECT_GE(p["x"].get<int>(), 0);
        EXPECT_LT(p["x"].get<int>(), 16);
        if (p["team"] == "npc") {
            EXPECT_EQ(p["y"], 2);
            xs.insert(p["x"].get<int>());
        } else {
            EXPECT_EQ(p["y"], 9);
        }
    }
    EXPECT_EQ(xs.size(), 3u);  // nobody stands on anybody
}

TEST(SceneLayout, TheDefaultCameraFacesTheNearestPerson) {
    const Json s = BuildScene(Exploring("Thorin", {Member("thorin", "Thorin", "fighter")}, {Npc("a", "Ann", "innkeeper")}));
    const Camera c = Camera::FromJson(s["camera"]);
    EXPECT_LT(std::min(c.yaw, 360 - c.yaw), 20);  // she is roughly straight up the field
    EXPECT_NEAR(c.z, 1.06, 1e-9);
    EXPECT_EQ(c.fov, 70);
}

namespace {
Json Fight(const char* hero, std::vector<Json> actors) {
    Json a = Json::array();
    for (auto& x : actors) a.push_back(x);
    return Json{{"mode", "combat"}, {"hero", hero}, {"party", Json::array({Member("thorin", "Thorin", "fighter"), Member("elara", "Elara", "wizard")})},
                {"combat", Json{{"grid", Json{{"width", 6}, {"height", 5}, {"rows", Json::array({"......", "..#...", "......", "......", "......"})}}}, {"actors", a}}}};
}
Json Actor(int id, const char* team, const char* name, const char* cls, const char* mon, int x, int y, const char* life = "alive") {
    return Json{{"id", id}, {"team", team}, {"name", name}, {"classId", cls}, {"monsterId", mon}, {"pos", Json::array({x, y})}, {"life", life}};
}
}  // namespace

TEST(SceneLayout, ACombatSceneKeepsTheRealGridAndSquares) {
    const Json s = BuildScene(Fight("Thorin", {Actor(0, "party", "Thorin", "fighter", "", 0, 2), Actor(1, "monsters", "Zombie 1", "", "zombie", 5, 2)}));
    EXPECT_EQ(s["grid"]["width"], 6);
    EXPECT_EQ(s["grid"]["rows"][1], "..#...");
    EXPECT_EQ(s["props"][0]["id"], "thorin");  // a party member's character id, not its actor number
    EXPECT_EQ(s["props"][1]["id"], "zombie");
    EXPECT_EQ(s["props"][1]["team"], "foe");
    EXPECT_EQ(s["props"][1]["kind"], "zombie");
}

TEST(SceneLayout, TheCameraTurnsToTheNearestFoeAndSkipsTheDead) {
    const Json s = BuildScene(Fight("Thorin", {Actor(0, "party", "Thorin", "fighter", "", 0, 2), Actor(1, "monsters", "Far", "", "ogre", 5, 2), Actor(2, "monsters", "Near", "", "zombie", 0, 0),
                                               Actor(3, "monsters", "Dead", "", "ghoul", 1, 2, "dead")}));
    const Camera c = Camera::FromJson(s["camera"]);
    EXPECT_NEAR(c.yaw, 0, 0.01);  // Near is straight up the grid; Dead is closer but dead
}

TEST(SceneLayout, WithoutFoesTheCameraFacesNorth) {
    const Json s = BuildScene(Fight("Thorin", {Actor(0, "party", "Thorin", "fighter", "", 3, 3)}));
    EXPECT_EQ(Camera::FromJson(s["camera"]).yaw, 0);
}

TEST(SceneLayout, TheHeroIsOnlyEverOneActor) {
    const Json s = BuildScene(Fight("Elara", {Actor(0, "party", "Thorin", "fighter", "", 1, 3), Actor(1, "party", "Elara", "wizard", "", 2, 3), Actor(2, "monsters", "Zombie", "", "zombie", 2, 0)}));
    EXPECT_EQ(s["hero"]["id"], "elara");
    EXPECT_FALSE(s["props"][0]["hero"].get<bool>());
    EXPECT_TRUE(s["props"][1]["hero"].get<bool>());
    const Camera c = Camera::FromJson(s["camera"]);
    EXPECT_NEAR(c.x, 2.5, 0.01);
    EXPECT_NEAR(c.y, 3.5, 0.01);
}

TEST(SceneLayout, MalformedViewsDoNotCrash) {
    EXPECT_NO_THROW(BuildScene(Json::object()));
    EXPECT_NO_THROW(BuildScene(Json{{"mode", "combat"}, {"combat", Json{{"grid", "oops"}, {"actors", 5}}}, {"party", "x"}}));
    EXPECT_NO_THROW(BuildScene(Json{{"combat", Json{{"grid", Json{{"width", 3}}}, {"actors", Json::array({Json{{"team", "party"}}})}}}}));
    EXPECT_NO_THROW(BuildScene(Json(nullptr)));
}

// ---------------------------------------------------------------- DescribeView

TEST(DescribeView, OpensWithTheCameraThenThePlace) {
    const std::string s = DescribeView(SceneOf({}, "The Hall", "A cold hall. Rats scurry."), At(5.5, 9.5));
    EXPECT_EQ(s.rfind("eye level shot, The Hall: A cold hall", 0), 0u) << s;
    EXPECT_FALSE(Has(s, "Rats"));  // the first sentence only
    EXPECT_TRUE(Has(s, "nobody in view"));
}

TEST(DescribeView, LongPlacesAreCut) {
    const std::string s = DescribeView(SceneOf({}, "Hall", std::string(400, 'x').c_str()), At(0, 0));
    EXPECT_LT(s.size(), 300u);
}

TEST(DescribeView, SurvivesAnEmptyPlace) {
    const std::string s = DescribeView(SceneOf({}, "", ""), At(0, 0));
    EXPECT_FALSE(Has(s, ", ,"));
    EXPECT_NE(s.rfind(", ", 0), 0u);
}

struct BandCase {
    double y;
    const char* band;
};
class DepthBands : public ::testing::TestWithParam<BandCase> {};
TEST_P(DepthBands, NamesHowFarAwayTheyAre) {
    const std::string s = DescribeView(SceneOf({Prop("Zombie 1", "foe", "zombie", 5, GetParam().y)}), At(5.5, 9.5));
    EXPECT_TRUE(Has(s, std::string("a zombie ") + GetParam().band)) << s;
}
INSTANTIATE_TEST_SUITE_P(Distances, DepthBands, ::testing::Values(BandCase{7, "in the foreground"}, BandCase{4, "in the middle distance"}, BandCase{-3, "in the distance"}));

struct SideCase {
    double x;
    const char* side;
};
class Sides : public ::testing::TestWithParam<SideCase> {};
TEST_P(Sides, NamesWhichSideOfThePicture) {
    const std::string s = DescribeView(SceneOf({Prop("Zombie 1", "foe", "zombie", GetParam().x, 4)}), At(5.5, 9.5));
    EXPECT_TRUE(Has(s, GetParam().side)) << s;
}
INSTANTIATE_TEST_SUITE_P(Positions, Sides, ::testing::Values(SideCase{2, "on the left"}, SideCase{5, "in the center"}, SideCase{9, "on the right"}));

TEST(DescribeView, GroupsIdenticalCreaturesAndCountsThem) {
    const std::string s = DescribeView(SceneOf({Prop("Zombie 1", "foe", "zombie", 4, 4), Prop("Zombie 2", "foe", "zombie", 5, 4), Prop("Zombie #3", "foe", "zombie", 6, 4)}), At(5.5, 9.5));
    EXPECT_TRUE(Has(s, "three zombies")) << s;
    EXPECT_FALSE(Has(s, "a zombie"));
}

TEST(DescribeView, NamesFriendsAndTheirClass) {
    const std::string s = DescribeView(SceneOf({Prop("Aldric", "party", "cleric", 5, 8)}), At(5.5, 9.5));
    EXPECT_TRUE(Has(s, "Aldric, a cleric in the foreground")) << s;
    const std::string t = DescribeView(SceneOf({Prop("Marta", "npc", "innkeeper", 5, 6)}), At(5.5, 9.5));
    EXPECT_TRUE(Has(t, "Marta, an innkeeper")) << t;
}

TEST(DescribeView, TheFallenLieDownAndTheDeadAreGone) {
    const std::string s = DescribeView(SceneOf({Prop("Finn", "party", "rogue", 5, 6, "unconscious"), Prop("Ghoul", "foe", "ghoul", 4, 6, "dead")}), At(5.5, 9.5));
    EXPECT_TRUE(Has(s, "lying fallen")) << s;
    EXPECT_FALSE(Has(s, "ghoul"));
}

TEST(DescribeView, OnlyWhatIsInFrameIsDescribed) {
    const Json sc = SceneOf({Prop("Ghoul", "foe", "ghoul", 5, 5), Prop("Wight", "foe", "wight", 5, 14), Prop("Ogre", "foe", "ogre", 25, 9)});
    const std::string s = DescribeView(sc, At(5.5, 9.5));
    EXPECT_TRUE(Has(s, "ghoul"));
    EXPECT_FALSE(Has(s, "wight"));  // behind the camera
    EXPECT_FALSE(Has(s, "ogre"));   // far off to the side
    EXPECT_TRUE(Has(DescribeView(sc, At(5.5, 9.5, 180)), "wight"));
}

TEST(DescribeView, AtMostSixKindsAreNamed) {
    std::vector<Json> props;
    const char* kinds[] = {"ghoul", "wight", "ogre", "troll", "wraith", "imp", "harpy", "gnoll"};
    for (int i = 0; i < 8; ++i) props.push_back(Prop(kinds[i], "foe", kinds[i], 3 + i * 0.5, 3 + (i % 3)));
    const std::string s = DescribeView(SceneOf(props), At(5.5, 12.5, 0, 0, 100));
    EXPECT_EQ(CountMatches(s, "\\b(foreground|distance)\\b"), 6) << s;
}

TEST(DescribeView, TheOwnerOfTheEyesIsNotInTheirOwnPicture) {
    const Json sc = SceneOf({Prop("Thorin", "party", "fighter", 5, 9, "alive", true), Prop("Elara", "party", "wizard", 5, 6)});
    EXPECT_FALSE(Has(DescribeView(sc, At(5.5, 9.5)), "Thorin"));
    EXPECT_TRUE(Has(DescribeView(sc, At(5.5, 12.5)), "Thorin"));  // seen from three squares behind
}

TEST(DescribeView, GivesTheSameWordsForTheSameInputs) {
    const Json sc = SceneOf({Prop("Zombie 1", "foe", "zombie", 4, 4), Prop("Aldric", "party", "cleric", 7, 6)});
    EXPECT_EQ(DescribeView(sc, At(5.5, 9.5, 5)), DescribeView(sc, At(5.5, 9.5, 5)));
}

TEST(DescribeView, ChangesWithTheCamera) {
    const Json sc = SceneOf({Prop("Zombie 1", "foe", "zombie", 4, 4)});
    EXPECT_NE(DescribeView(sc, At(5.5, 9.5, 0, 0)), DescribeView(sc, At(5.5, 9.5, 0, -50)));
    EXPECT_NE(DescribeView(sc, At(5.5, 9.5, 0, 0, 70)), DescribeView(sc, At(5.5, 9.5, 0, 0, 25)));
}
