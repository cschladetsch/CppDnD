#pragma once
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "dnd/Json.hpp"

namespace dnd {

// When something was recorded: the game's day and clock hour.
struct Stamp {
    int day = 1;
    int hour = 0;
};

struct HistoryEntry {
    int seq = 0;          // strictly increasing within one History
    Stamp when;
    std::string speaker;  // who said or did it
    std::string dir;      // pair files only: "to" (owner acted on the other) or "from" (the other acted on owner)
    std::string kind;     // say | tell | ask | reply | attack | spell | heal | down | death | proposal | narrate | scene | travel | quest | combat
    std::string text;
};

// A record of everything said and done between actors.
//
//  * Pair history: for every ordered pair (owner, with) a private list kept from the owner's point of view.
//    When x acts on or speaks to y, x's list about y holds it as dir "to" and y's list about x holds it as
//    dir "from". Nobody else's list sees it. On disk this is one file per ordered pair, "<owner>-<with>.json".
//  * Group history: what was said or announced to the whole table. On disk: "group-history.json".
//
// Memory is the source of truth (it saves with the game); files are rewritten from it when SetDir is used.
class History {
public:
    History() = default;
    History(const History&) = default;
    History(History&&) = default;
    History& operator=(const History&) = default;
    History& operator=(History&&) = default;
    ~History() { Flush(); }  // files are current when the game ends

    void Group(const std::string& speaker, const std::string& kind, const std::string& text, Stamp when);
    void Between(const std::string& from, const std::string& to, const std::string& kind, const std::string& text, Stamp when);

    // x-y.json as JSON: {"owner","with","entries":[...]}, entries after sinceSeq, at most `limit` (the newest).
    Json Pair(const std::string& owner, const std::string& with, int sinceSeq = 0, int limit = 200) const;
    Json GroupJson(int sinceSeq = 0, int limit = 200) const;
    // {"group": n, "pairs": [{"owner","with","count","file"}]}
    Json Index() const;
    // A small digest for an agent's prompt: the last lines of group talk and of its own conversations.
    Json Recent(const std::string& owner, int groupLines = 8, int pairLines = 4, int maxPartners = 3) const;

    Json Dump() const;
    static History Restore(const Json& j);

    // Files. SetDir writes everything now and after each later Flush; "" turns files off.
    void SetDir(const std::string& dir);
    const std::string& Dir() const { return dir_; }
    bool Flush(std::string* err = nullptr);

    static std::string PairFile(const std::string& owner, const std::string& with);
    static std::string GroupFile() { return "group-history.json"; }

private:
    struct Thread {
        std::string owner, with;
        std::vector<HistoryEntry> entries;
    };
    static std::string Key(const std::string& owner, const std::string& with);
    static Json EntryJson(const HistoryEntry& e, bool pair);
    static void Push(std::vector<HistoryEntry>& v, HistoryEntry e);
    Json ThreadJson(const Thread& t, int sinceSeq, int limit) const;

    std::map<std::string, Thread> pairs_;
    std::vector<HistoryEntry> group_;
    int seq_ = 0;
    std::string dir_;
    std::set<std::string> dirty_;
    bool groupDirty_ = false;
};

}  // namespace dnd
