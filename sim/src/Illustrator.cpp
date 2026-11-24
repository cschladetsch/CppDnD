#include "dnd/Illustrator.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>

#include "dnd/Util.hpp"

namespace dnd {
namespace fs = std::filesystem;
namespace {

std::string Str(const Json& j, const char* k) { return j.is_object() && j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : ""; }

// One line of plain words: whitespace collapsed, cut at a word boundary to at most cap characters.
std::string OneLine(const std::string& s, std::size_t cap) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) { space = !out.empty(); continue; }
        if (space) out.push_back(' ');
        space = false;
        out.push_back(c);
    }
    if (out.size() > cap) {
        auto cut = out.rfind(' ', cap);
        out.resize(cut == std::string::npos ? cap : cut);
    }
    return out;
}

// Events that tell the story. Rounds, turns, moves and death saves are bookkeeping; everything else is kept, with the
// narrator's prose where there is some.
bool StoryEvent(const std::string& type) {
    static const std::set<std::string> skip = {"round_start", "turn_start", "move", "death_save"};
    return !skip.count(type);
}

}  // namespace

// ---------------------------------------------------------------- digest

Json StoryDigest::ToJson() const { return Json{{"text", text}, {"lastSeq", lastSeq}, {"updates", updates}}; }

StoryDigest StoryDigest::FromJson(const Json& j) {
    StoryDigest d;
    if (!j.is_object()) return d;
    d.text = Str(j, "text");
    d.lastSeq = j.contains("lastSeq") && j["lastSeq"].is_number_integer() ? j["lastSeq"].get<int>() : 0;
    d.updates = j.contains("updates") && j["updates"].is_number_integer() ? j["updates"].get<int>() : 0;
    return d;
}

void StoryState::LoadIfNeeded() {
    if (loaded) return;
    loaded = true;
    if (dir.empty()) return;
    std::ifstream in(fs::path(dir) / "story.json", std::ios::binary);
    if (!in) return;
    const Json j = Json::parse(in, nullptr, false);
    if (!j.is_discarded()) digest = StoryDigest::FromJson(j);
}

void StoryState::Save() const {
    if (dir.empty()) return;
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path file = fs::path(dir) / "story.json", tmp = fs::path(dir) / "story.json.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << digest.ToJson().dump(2) << "\n";
    }
    fs::rename(tmp, file, ec);
    if (ec) { fs::remove(file, ec); fs::rename(tmp, file, ec); }
}

// ---------------------------------------------------------------- material

StoryMaterial GatherStory(const Json& view, const std::string& cameraWords, bool mature, std::size_t maxLines) {
    StoryMaterial m;
    m.mature = mature;
    m.camera = cameraWords;
    m.lastSeq = view.contains("lastSeq") && view["lastSeq"].is_number_integer() ? view["lastSeq"].get<int>() : 0;
    if (view.contains("events") && view["events"].is_array())
        for (const auto& e : view["events"]) {
            if (!e.is_object() || !StoryEvent(Str(e, "type"))) continue;
            std::string line = Str(e, "flavour");
            if (line.empty()) line = Str(e, "text");
            line = OneLine(line, 400);
            if (line.empty()) continue;
            const int seq = e.contains("seq") && e["seq"].is_number_integer() ? e["seq"].get<int>() : 0;
            m.lines.push_back({seq, line});
        }
    if (m.lines.size() > maxLines) m.lines.erase(m.lines.begin(), m.lines.end() - static_cast<std::ptrdiff_t>(maxLines));

    const Json loc = view.contains("location") && view["location"].is_object() ? view["location"] : Json::object();
    m.place = Str(loc, "name");
    if (const std::string d = OneLine(Str(loc, "description"), 300); !d.empty()) m.place += (m.place.empty() ? "" : ": ") + d;

    if (view.contains("party") && view["party"].is_array())
        for (const auto& p : view["party"]) {
            if (!p.is_object()) continue;
            std::string line = Str(p, "name");
            std::vector<std::string> what;
            if (const std::string g = Str(p, "gender"); !g.empty()) what.push_back(g);
            if (p.contains("tags") && p["tags"].is_array())
                for (const auto& t : p["tags"])
                    if (t.is_string() && t.get<std::string>() != "humanoid") what.push_back(t.get<std::string>());
            if (const std::string c = Str(p, "class"); !c.empty()) what.push_back(c);
            const bool dead = p.contains("dead") && p["dead"].is_boolean() && p["dead"].get<bool>();
            const int hp = p.contains("hp") && p["hp"].is_number() ? p["hp"].get<int>() : -1, maxHp = p.contains("maxHp") && p["maxHp"].is_number() ? p["maxHp"].get<int>() : -1;
            if (dead) what.push_back("dead");
            else if (hp == 0) what.push_back("unconscious");
            else if (hp > 0 && maxHp > 0 && hp * 2 < maxHp) what.push_back("badly wounded");
            std::string w;
            for (const auto& x : what) w += (w.empty() ? "" : ", ") + x;
            if (!w.empty()) line += " (" + w + ")";
            if (Str(view, "hero") == Str(p, "name")) line += ", the hero, whose eyes are the camera unless it says otherwise";
            m.party += (m.party.empty() ? "" : "\n") + std::string("- ") + line;
        }
    return m;
}

// ---------------------------------------------------------------- illustrator

Illustrator::Illustrator(std::shared_ptr<ILlmClient> client, Config cfg) : client_(std::move(client)), cfg_(cfg) {}

std::string Illustrator::System(bool mature) const {
    std::string s = Cat(
        "You keep the picture book of a tabletop role-playing story and you have two jobs.\n"
        "1. \"story\": rewrite the story so far as one condensed history of at most ", cfg_.storyWords, " words. Keep who each person is and "
        "exactly what they look like (sex, race, age, build, skin, hair, face), what they wear or carry and what state it is in, their wounds, "
        "where everyone is, what has happened, and what is happening at this moment. Keep the facts of the old history unless the new events "
        "change them; when they disagree the newest event wins. Plain prose, past tense for what happened and present tense for now.\n"
        "2. \"prompt\": a Stable Diffusion prompt for a realistic photograph of this moment as the camera sees it. Comma-separated short "
        "phrases, the most important first: the people in frame and what they are doing, their faces, bodies, hair, clothing and expressions, "
        "then the place, the light and time of day, the weather, the mood. Describe people by how they look, never by name (the image model "
        "does not know the names). Use concrete visual words a photographer would; leave out sounds, smells, thoughts, game terms, dice and "
        "numbers. No quotes, brackets or words like painting or illustration. At most ", cfg_.promptWords, " words.\n"
        "Reply with JSON only: {\"story\": \"...\", \"prompt\": \"...\"}");
    if (mature)
        s += "\nThis is an adult story: follow it frankly, including nudity and sex when the story has them; clothes are on unless the story "
             "says otherwise. Every person is an adult: call them women and men (adult woman, adult man), never girls, boys or anything younger.";
    return s;
}

std::string Illustrator::User(const StoryDigest& digest, const StoryMaterial& m, bool& rebuilt, int& newest) {
    rebuilt = m.lastSeq < digest.lastSeq || digest.text.empty();  // rewound, undone, or the first picture of this game
    const int since = rebuilt ? 0 : digest.lastSeq;
    newest = rebuilt ? 0 : digest.lastSeq;
    std::string fresh;
    std::size_t n = 0;
    for (const auto& l : m.lines) {
        if (l.seq <= since) continue;
        fresh += "- " + l.text + "\n";
        newest = std::max(newest, l.seq);
        ++n;
    }
    std::string u;
    u += "THE STORY SO FAR (your last condensed history):\n";
    u += rebuilt || digest.text.empty() ? std::string("(nothing yet)") : digest.text;
    u += "\n\nNEW EVENTS, oldest first:\n";
    u += n ? fresh : std::string("(none)\n");
    if (!m.party.empty()) u += "\nTHE PARTY:\n" + m.party + "\n";
    if (!m.place.empty()) u += "\nWHERE THEY ARE NOW: " + m.place + "\n";
    if (!m.camera.empty()) u += "\nTHE CAMERA: " + m.camera + "\n";
    u += "\nWrite the new condensed history and the picture's prompt.";
    return u;
}

std::string Illustrator::CleanPrompt(const std::string& s, int words) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == '"' || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' || c == '<' || c == '>' || c == ':' || c == '|') continue;
        if (std::isspace(static_cast<unsigned char>(c))) { space = !out.empty(); continue; }
        if (space) out.push_back(' ');
        space = false;
        out.push_back(c);
    }
    int n = 0;
    for (std::size_t i = 0; i < out.size(); ++i)
        if (out[i] == ' ' && ++n >= words) {
            out.resize(i);
            break;
        }
    while (!out.empty() && (out.back() == ',' || out.back() == '.' || out.back() == ' ')) out.pop_back();
    return out;
}

bool Illustrator::Parse(const std::string& reply, std::string& story, std::string& prompt, std::string& err) {
    const std::string obj = ExtractJsonObject(StripThink(reply));
    const Json j = Json::parse(obj.empty() ? reply : obj, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { err = "the illustrator did not answer with JSON"; return false; }
    story = OneLine(Str(j, "story"), 4000);
    prompt = Str(j, "prompt");
    if (prompt.empty()) { err = "the illustrator wrote no prompt"; return false; }
    return true;
}

Illustrator::Shot Illustrator::Compose(StoryDigest& digest, const StoryMaterial& m) {
    Shot shot;
    if (!client_) { shot.error = "no illustrator model"; return shot; }
    bool rebuilt = false;
    int newest = 0;
    const std::string user = User(digest, m, rebuilt, newest);
    const LlmReply r = client_->Chat(System(m.mature), user, true);
    if (!r.ok) { shot.error = r.error.empty() ? "the illustrator model failed" : r.error; return shot; }
    std::string story, prompt;
    if (!Parse(r.content, story, prompt, shot.error)) return shot;
    shot.prompt = CleanPrompt(prompt, cfg_.promptWords);
    if (shot.prompt.empty()) { shot.error = "the illustrator wrote no prompt"; return shot; }
    if (!story.empty()) {
        digest.text = story;
        ++digest.updates;
    } else if (rebuilt) digest.text.clear();
    digest.lastSeq = std::max(newest, rebuilt ? m.lastSeq : digest.lastSeq);
    shot.ok = true;
    return shot;
}

}  // namespace dnd
