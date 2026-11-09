#include "dnd/Scene.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <vector>

namespace dnd {
namespace {

constexpr double kPi = 3.14159265358979323846;
double Rad(double d) { return d * kPi / 180.0; }

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

double Num(const Json& j, const char* key, double fallback) {
    if (!j.is_object()) return fallback;
    const auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<double>() : fallback;
}

std::string Str(const Json& j, const char* key) {
    if (!j.is_object()) return "";
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : "";
}

std::string Spaced(std::string s) {
    for (char& c : s)
        if (c == '_') c = ' ';
    return s;
}

// "Zombie 2" and "Zombie #2" are both a zombie.
std::string StripNumber(std::string s) {
    while (!s.empty() && (std::isdigit(static_cast<unsigned char>(s.back())) || s.back() == '#' || s.back() == ' ')) s.pop_back();
    return s;
}

std::string Article(const std::string& w) { return !w.empty() && std::string("aeiouAEIOU").find(w[0]) != std::string::npos ? "an" : "a"; }

std::string CountWord(int n) {
    static const char* w[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten"};
    return n >= 0 && n <= 10 ? w[n] : std::to_string(n);
}

struct Basis {
    double f[3], r[3], u[3];
};
Basis BasisOf(const Camera& c) {
    const double y = Rad(c.yaw), p = Rad(c.pitch);
    Basis b;
    b.f[0] = std::sin(y) * std::cos(p);
    b.f[1] = -std::cos(y) * std::cos(p);
    b.f[2] = std::sin(p);
    b.r[0] = std::cos(y);
    b.r[1] = std::sin(y);
    b.r[2] = 0;
    b.u[0] = b.f[1] * b.r[2] - b.f[2] * b.r[1];
    b.u[1] = b.f[2] * b.r[0] - b.f[0] * b.r[2];
    b.u[2] = b.f[0] * b.r[1] - b.f[1] * b.r[0];
    return b;
}

}  // namespace

// ---------------------------------------------------------------- camera

void Camera::Clamp() {
    auto fin = [](double v, double lo, double hi, double dflt) { return std::isfinite(v) ? std::clamp(v, lo, hi) : dflt; };
    x = fin(x, -400, 800, 0);
    y = fin(y, -400, 800, 0);
    z = fin(z, 0.05, 60, 1.06);
    pitch = fin(pitch, -89, 89, 0);
    fov = fin(fov, 15, 120, 70);
    yaw = std::isfinite(yaw) ? std::fmod(std::fmod(yaw, 360.0) + 360.0, 360.0) : 0;
}

Camera Camera::FromJson(const Json& j, const Camera& fallback) {
    Camera c = fallback;
    c.x = Num(j, "x", c.x);
    c.y = Num(j, "y", c.y);
    c.z = Num(j, "z", c.z);
    c.yaw = Num(j, "yaw", c.yaw);
    c.pitch = Num(j, "pitch", c.pitch);
    c.fov = Num(j, "fov", c.fov);
    c.Clamp();
    return c;
}

Camera Camera::FromJson(const Json& j) { return FromJson(j, Camera()); }

Json Camera::ToJson() const {
    auto r = [](double v) { return std::round(v * 100.0) / 100.0; };
    return Json{{"x", r(x)}, {"y", r(y)}, {"z", r(z)}, {"yaw", r(yaw)}, {"pitch", r(pitch)}, {"fov", r(fov)}};
}

bool Project(const Camera& cam, double aspect, double wx, double wy, double wz, double& sx, double& sy, double& depth) {
    const Basis b = BasisOf(cam);
    const double d[3] = {wx - cam.x, wy - cam.y, wz - cam.z};
    depth = d[0] * b.f[0] + d[1] * b.f[1] + d[2] * b.f[2];
    if (depth <= 0.05) return false;
    const double cx = d[0] * b.r[0] + d[1] * b.r[1] + d[2] * b.r[2];
    const double cy = d[0] * b.u[0] + d[1] * b.u[1] + d[2] * b.u[2];
    const double th = std::tan(Rad(cam.fov) / 2.0);
    sy = cy / (depth * th);
    sx = cx / (depth * th * (aspect > 0.1 ? aspect : 1.0));
    return true;
}

// ---------------------------------------------------------------- scene

Json BuildScene(const Json& view) {
    Json scene;
    scene["mode"] = Str(view, "mode");
    const Json combat = view.contains("combat") && view["combat"].is_object() ? view["combat"] : Json();
    const std::string heroKey = Lower(Str(view, "hero"));

    Json props = Json::array();
    Json rows = Json::array();
    int W = 16, H = 12;
    if (combat.is_object() && combat.contains("grid")) {
        const Json& g = combat["grid"];
        W = static_cast<int>(Num(g, "width", W));
        H = static_cast<int>(Num(g, "height", H));
        if (g.contains("rows") && g["rows"].is_array()) rows = g["rows"];
        std::map<std::string, std::string> charIds;  // party member name -> character id
        if (view.contains("party") && view["party"].is_array())
            for (const auto& p : view["party"]) charIds[Lower(Str(p, "name"))] = Str(p, "id");
        if (combat.contains("actors") && combat["actors"].is_array())
            for (const auto& a : combat["actors"]) {
                const bool party = Str(a, "team") == "party";
                int x = 0, y = 0;
                if (a.contains("pos") && a["pos"].is_array() && a["pos"].size() >= 2 && a["pos"][0].is_number() && a["pos"][1].is_number()) {
                    x = a["pos"][0].get<int>();
                    y = a["pos"][1].get<int>();
                }
                const std::string name = Str(a, "name");
                const std::string id = party && charIds.count(Lower(name)) ? charIds[Lower(name)] : Str(a, "monsterId");
                props.push_back(Json{{"id", id.empty() ? name : id}, {"name", name}, {"team", party ? "party" : "foe"},
                                     {"kind", party ? Str(a, "classId") : Str(a, "monsterId")}, {"x", x}, {"y", y}, {"life", Str(a, "life").empty() ? "alive" : Str(a, "life")},
                                     {"hero", false}});
            }
    } else {
        for (int y = 0; y < H; ++y) rows.push_back(std::string(static_cast<std::size_t>(W), '.'));
        std::vector<Json> party, npcs;
        if (view.contains("party") && view["party"].is_array())
            for (const auto& p : view["party"])
                if (!(p.contains("dead") && p["dead"].is_boolean() && p["dead"].get<bool>())) party.push_back(p);
        if (view.contains("location") && view["location"].contains("npcs") && view["location"]["npcs"].is_array())
            for (const auto& n : view["location"]["npcs"]) npcs.push_back(n);
        auto spread = [&](const std::vector<Json>& v, int y, const char* team, bool isParty) {
            const int n = static_cast<int>(v.size());
            for (int i = 0; i < n; ++i) {
                const int x = std::clamp(static_cast<int>(std::floor(W / 2.0 + (i - (n - 1) / 2.0) * 2.0)), 0, W - 1);
                props.push_back(Json{{"id", Str(v[static_cast<std::size_t>(i)], "id")}, {"name", Str(v[static_cast<std::size_t>(i)], "name")}, {"team", team},
                                     {"kind", isParty ? Str(v[static_cast<std::size_t>(i)], "class") : Str(v[static_cast<std::size_t>(i)], "type")}, {"x", x}, {"y", y}, {"life", "alive"}, {"hero", false}});
            }
        };
        spread(party, H - 3, "party", true);
        spread(npcs, 2, "npc", false);
    }

    // who is the hero
    int heroAt = -1;
    for (std::size_t i = 0; i < props.size(); ++i)
        if (Str(props[i], "team") == "party" && (Lower(Str(props[i], "name")) == heroKey || Lower(Str(props[i], "id")) == heroKey)) { heroAt = static_cast<int>(i); break; }
    if (heroAt < 0)
        for (std::size_t i = 0; i < props.size(); ++i)
            if (Str(props[i], "team") == "party") { heroAt = static_cast<int>(i); break; }
    if (heroAt >= 0) props[static_cast<std::size_t>(heroAt)]["hero"] = true;

    scene["grid"] = Json{{"width", W}, {"height", H}, {"rows", rows}};
    scene["props"] = props;
    scene["hero"] = heroAt >= 0 ? Json{{"id", Str(props[static_cast<std::size_t>(heroAt)], "id")}, {"name", Str(props[static_cast<std::size_t>(heroAt)], "name")}} : Json(nullptr);

    const Json loc = view.contains("location") && view["location"].is_object() ? view["location"] : Json::object();
    scene["location"] = Json{{"name", Str(loc, "name")}, {"description", Str(loc, "description")}, {"kind", Str(loc, "kind")}};

    // default camera: the hero's eyes, turned toward the nearest foe, or else the nearest other person
    Camera cam;
    cam.x = W / 2.0;
    cam.y = H / 2.0;
    if (heroAt >= 0) {
        const Json& h = props[static_cast<std::size_t>(heroAt)];
        cam.x = Num(h, "x", 0) + 0.5;
        cam.y = Num(h, "y", 0) + 0.5;
        double best = 1e18;
        bool found = false;
        for (const auto& p : props) {
            const std::string team = Str(p, "team");
            if (team == "party" || Str(p, "life") == "dead") continue;
            const double dx = Num(p, "x", 0) + 0.5 - cam.x, dy = Num(p, "y", 0) + 0.5 - cam.y, d = dx * dx + dy * dy;
            if (d < best) { best = d; cam.yaw = std::atan2(dx, -dy) * 180.0 / kPi; found = true; }
        }
        if (!found) cam.yaw = 0;
    }
    cam.pitch = -3;
    cam.Clamp();
    scene["camera"] = cam.ToJson();
    return scene;
}

// ---------------------------------------------------------------- words

std::string CameraWords(const Camera& cam) {
    std::vector<std::string> w;
    const double p = cam.pitch;
    if (p <= -60) w.push_back("top-down view, bird's eye view, looking straight down");
    else if (p <= -25) w.push_back("high angle shot, looking down");
    else if (p <= -9) w.push_back("slightly high angle");
    else if (p < 9) w.push_back("eye level shot");
    else if (p < 25) w.push_back("slightly low angle");
    else if (p < 50) w.push_back("low angle shot, looking up");
    else w.push_back("extreme low angle, worm's eye view, looking up");
    if (cam.z > 3.0 && p > -60) w.push_back("seen from high above");
    if (cam.fov <= 30) w.push_back("telephoto lens, compressed perspective, shallow depth of field");
    else if (cam.fov <= 50) w.push_back("narrow field of view, portrait lens");
    else if (cam.fov > 100) w.push_back("ultra wide angle lens, strong perspective distortion");
    else if (cam.fov > 80) w.push_back("wide angle lens");
    std::string s;
    for (const auto& x : w) s += (s.empty() ? "" : ", ") + x;
    return s;
}

std::string DescribeView(const Json& scene, const Camera& cam, double aspect) {
    std::string out = CameraWords(cam);
    auto add = [&](const std::string& part) {
        if (part.empty()) return;
        out += (out.empty() ? "" : ", ") + part;
    };

    const Json loc = scene.contains("location") ? scene["location"] : Json::object();
    std::string where = Str(loc, "name");
    std::string desc = Str(loc, "description");
    if (const auto dot = desc.find(". "); dot != std::string::npos) desc.resize(dot);
    if (desc.size() > 160) desc.resize(160);
    if (!desc.empty()) where += (where.empty() ? "" : ": ") + desc;
    add(where);

    struct Seen {
        std::string label, plural, band, side, life;
        int count = 0;
        double depth = 1e18;
    };
    std::map<std::string, Seen> groups;
    if (scene.contains("props") && scene["props"].is_array())
        for (const auto& p : scene["props"]) {
            if (Str(p, "life") == "dead") continue;
            const double px = Num(p, "x", 0) + 0.5, py = Num(p, "y", 0) + 0.5;
            const bool hero = p.contains("hero") && p["hero"].is_boolean() && p["hero"].get<bool>();
            if (hero && std::hypot(px - cam.x, py - cam.y) < 0.8) continue;  // that is the camera's own owner
            double sx, sy, depth;
            if (!Project(cam, aspect, px, py, 0.55, sx, sy, depth)) continue;
            if (std::fabs(sx) > 1.05 || std::fabs(sy) > 1.05) continue;
            const std::string team = Str(p, "team"), name = Str(p, "name"), kind = Spaced(Str(p, "kind"));
            std::string label, base;
            if (team == "foe") {
                base = Lower(StripNumber(Spaced(name.empty() ? kind : name)));
                label = Article(base) + " " + base;
            } else {
                label = name + ", " + Article(kind.empty() ? "person" : kind) + " " + (kind.empty() ? "person" : kind);
                base = label;
            }
            const std::string band = depth < 3 ? "in the foreground" : depth < 8 ? "in the middle distance" : "in the distance";
            const std::string side = sx < -0.35 ? " on the left" : sx > 0.35 ? " on the right" : " in the center";
            Seen& g = groups[team == "foe" ? base : label];
            ++g.count;
            if (depth < g.depth) { g.depth = depth; g.band = band; g.side = side; }
            g.label = label;
            g.plural = base + "s";
            g.life = Str(p, "life");
        }
    std::vector<Seen> ordered;
    for (auto& kv : groups) ordered.push_back(kv.second);
    std::sort(ordered.begin(), ordered.end(), [](const Seen& a, const Seen& b) { return a.depth < b.depth; });
    if (ordered.empty()) { add("nobody in view"); return out; }
    int n = 0;
    for (const auto& g : ordered) {
        if (++n > 6) break;
        std::string s = g.count > 1 ? CountWord(g.count) + " " + g.plural : g.label;
        if (g.life != "alive" && !g.life.empty() && g.count == 1) s += " lying fallen";
        add(s + " " + g.band + g.side);
    }
    return out;
}

}  // namespace dnd
