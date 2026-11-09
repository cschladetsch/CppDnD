#include "dnd/History.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace dnd {
namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string Slug(const std::string& s) {
    std::string out;
    for (char c : Lower(s)) out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    return out.empty() ? "_" : out;
}
constexpr std::size_t kMaxEntries = 2000, kTrim = 500;

HistoryEntry EntryFrom(const Json& j) {
    HistoryEntry e;
    e.seq = j.value("seq", 0);
    e.when = {j.value("day", 1), j.value("hour", 0)};
    e.speaker = j.value("speaker", "");
    e.dir = j.value("dir", "");
    e.kind = j.value("kind", "");
    e.text = j.value("text", "");
    return e;
}

}  // namespace

std::string History::Key(const std::string& owner, const std::string& with) { return Lower(owner) + "\n" + Lower(with); }
std::string History::PairFile(const std::string& owner, const std::string& with) { return Slug(owner) + "-" + Slug(with) + ".json"; }

void History::Push(std::vector<HistoryEntry>& v, HistoryEntry e) {
    v.push_back(std::move(e));
    if (v.size() > kMaxEntries) v.erase(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(kTrim));
}

void History::Group(const std::string& speaker, const std::string& kind, const std::string& text, Stamp when) {
    if (text.empty()) return;
    Push(group_, HistoryEntry{++seq_, when, speaker, "", kind, text});
    groupDirty_ = true;
}

void History::Between(const std::string& from, const std::string& to, const std::string& kind, const std::string& text, Stamp when) {
    if (text.empty() || from.empty() || to.empty() || Lower(from) == Lower(to)) return;
    const int n = ++seq_;
    auto add = [&](const std::string& owner, const std::string& with, const char* dir) {
        Thread& t = pairs_[Key(owner, with)];
        if (t.owner.empty()) { t.owner = owner; t.with = with; }
        Push(t.entries, HistoryEntry{n, when, from, dir, kind, text});
        dirty_.insert(Key(owner, with));
    };
    add(from, to, "to");
    add(to, from, "from");
}

Json History::EntryJson(const HistoryEntry& e, bool pair) {
    Json j{{"seq", e.seq}, {"day", e.when.day}, {"hour", e.when.hour}, {"speaker", e.speaker}, {"kind", e.kind}, {"text", e.text}};
    if (pair) j["dir"] = e.dir;
    return j;
}

Json History::ThreadJson(const Thread& t, int sinceSeq, int limit) const {
    Json entries = Json::array();
    for (const auto& e : t.entries)
        if (e.seq > sinceSeq) entries.push_back(EntryJson(e, true));
    while (limit > 0 && static_cast<int>(entries.size()) > limit) entries.erase(entries.begin());
    return Json{{"owner", t.owner}, {"with", t.with}, {"perspective", t.owner}, {"entries", entries}};
}

Json History::Pair(const std::string& owner, const std::string& with, int sinceSeq, int limit) const {
    auto it = pairs_.find(Key(owner, with));
    if (it == pairs_.end()) return Json{{"owner", owner}, {"with", with}, {"perspective", owner}, {"entries", Json::array()}};
    return ThreadJson(it->second, sinceSeq, limit);
}

Json History::GroupJson(int sinceSeq, int limit) const {
    Json entries = Json::array();
    for (const auto& e : group_)
        if (e.seq > sinceSeq) entries.push_back(EntryJson(e, false));
    while (limit > 0 && static_cast<int>(entries.size()) > limit) entries.erase(entries.begin());
    return Json{{"entries", entries}};
}

Json History::Index() const {
    Json pairs = Json::array();
    for (const auto& [k, t] : pairs_) pairs.push_back(Json{{"owner", t.owner}, {"with", t.with}, {"count", t.entries.size()}, {"file", PairFile(t.owner, t.with)}});
    return Json{{"group", group_.size()}, {"groupFile", GroupFile()}, {"pairs", pairs}, {"dir", dir_}};
}

Json History::Recent(const std::string& owner, int groupLines, int pairLines, int maxPartners) const {
    Json out = Json::object();
    Json g = Json::array();
    const std::size_t from = group_.size() > static_cast<std::size_t>(groupLines) ? group_.size() - static_cast<std::size_t>(groupLines) : 0;
    for (std::size_t i = from; i < group_.size(); ++i) g.push_back(Json{{"from", group_[i].speaker}, {"kind", group_[i].kind}, {"text", group_[i].text}});
    if (!g.empty()) out["group"] = g;

    // The partners this owner dealt with most recently.
    std::vector<const Thread*> mine;
    for (const auto& [k, t] : pairs_)
        if (Lower(t.owner) == Lower(owner) && !t.entries.empty()) mine.push_back(&t);
    std::sort(mine.begin(), mine.end(), [](const Thread* a, const Thread* b) { return a->entries.back().seq > b->entries.back().seq; });
    if (static_cast<int>(mine.size()) > maxPartners) mine.resize(static_cast<std::size_t>(maxPartners));
    Json with = Json::object();
    for (const Thread* t : mine) {
        Json lines = Json::array();
        const std::size_t f = t->entries.size() > static_cast<std::size_t>(pairLines) ? t->entries.size() - static_cast<std::size_t>(pairLines) : 0;
        for (std::size_t i = f; i < t->entries.size(); ++i) lines.push_back(Json{{"dir", t->entries[i].dir}, {"kind", t->entries[i].kind}, {"text", t->entries[i].text}});
        with[t->with] = lines;
    }
    if (!with.empty()) out["with"] = with;
    return out;
}

Json History::Dump() const {
    Json pairs = Json::array();
    for (const auto& [k, t] : pairs_) {
        Json es = Json::array();
        for (const auto& e : t.entries) es.push_back(EntryJson(e, true));
        pairs.push_back(Json{{"owner", t.owner}, {"with", t.with}, {"entries", es}});
    }
    Json g = Json::array();
    for (const auto& e : group_) g.push_back(EntryJson(e, false));
    return Json{{"seq", seq_}, {"pairs", pairs}, {"group", g}};
}

History History::Restore(const Json& j) {
    History h;
    if (!j.is_object()) return h;
    h.seq_ = j.value("seq", 0);
    if (j.contains("group"))
        for (const auto& e : j.at("group")) h.group_.push_back(EntryFrom(e));
    if (j.contains("pairs"))
        for (const auto& p : j.at("pairs")) {
            Thread t;
            t.owner = p.value("owner", "");
            t.with = p.value("with", "");
            if (t.owner.empty() || t.with.empty()) continue;
            for (const auto& e : p.at("entries")) t.entries.push_back(EntryFrom(e));
            h.pairs_[Key(t.owner, t.with)] = std::move(t);
        }
    return h;
}

void History::SetDir(const std::string& dir) {
    dir_ = dir;
    if (dir_.empty()) return;
    groupDirty_ = true;
    for (const auto& [k, t] : pairs_) dirty_.insert(k);
    Flush();
}

bool History::Flush(std::string* err) {
    if (dir_.empty() || (!groupDirty_ && dirty_.empty())) return true;
    namespace fs = std::filesystem;
    auto write = [&](const std::string& name, const Json& j) {
        const fs::path final = fs::path(dir_) / name, tmp = fs::path(dir_) / (name + ".part");
        {
            std::ofstream out(tmp, std::ios::binary);
            if (!out) return false;
            out << j.dump(2) << "\n";
            if (!out) return false;
        }
        std::error_code ec;
        fs::rename(tmp, final, ec);
        if (ec) { fs::remove(final, ec); fs::rename(tmp, final, ec); }
        return !ec;
    };
    try {
        std::error_code ec;
        fs::create_directories(dir_, ec);
        bool ok = true;
        if (groupDirty_) {
            Json j = GroupJson(0, 0);
            ok = write(GroupFile(), j) && ok;
        }
        for (const auto& k : dirty_) {
            auto it = pairs_.find(k);
            if (it != pairs_.end()) ok = write(PairFile(it->second.owner, it->second.with), ThreadJson(it->second, 0, 0)) && ok;
        }
        if (!ok) { if (err) *err = "could not write history files in " + dir_; return false; }
        groupDirty_ = false;
        dirty_.clear();
        return true;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}

}  // namespace dnd
