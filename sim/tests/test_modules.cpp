// Every module is a graph of places with flags as keys. These tests check, for each shipped module, that the graph hangs together
// and that the flags gating it can actually be earned, for every outcome of its seeded secrets, so a content edit cannot ship an
// unwinnable story.
#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <vector>

#include "dnd/Content.hpp"
#include "dnd/Game.hpp"
#include "test_support.hpp"

using namespace dnd;

namespace {

// "flag", "!flag", "flag=value", "!flag=value"
struct Cond {
    bool negate = false;
    std::string name, value;
    bool hasValue = false;
};
Cond ParseCond(std::string s) {
    Cond c;
    if (!s.empty() && s[0] == '!') { c.negate = true; s.erase(0, 1); }
    const auto eq = s.find('=');
    if (eq != std::string::npos) { c.hasValue = true; c.value = s.substr(eq + 1); s.erase(eq); }
    c.name = s;
    return c;
}

// The flags a game could hold: what is drawn at the start, plus what has been earned so far.
using Flags = std::map<std::string, std::string>;  // name -> value ("1" for plain flags)

bool Holds(const Flags& f, const std::string& cond) {
    const Cond c = ParseCond(cond);
    const auto it = f.find(c.name);
    const bool has = it != f.end() && (!c.hasValue || it->second == c.value);
    return c.negate ? !has : has;
}
bool AllHold(const Flags& f, const std::vector<std::string>& needs) {
    for (const auto& n : needs)
        if (!Holds(f, n)) return false;
    return true;
}
void Earn(Flags& f, const std::vector<std::string>& flags) {
    for (const auto& s : flags) f.emplace(s, "1");
}

struct Reach {
    std::set<std::string> places;
    Flags flags;
};
// Plays the module in the abstract: walk wherever the exits allow, trigger every event, hear every topic, repeat until nothing new
// happens. (Fights are assumed won; the autopilot test below checks they can be.)
Reach Closure(const ModuleDef& m, const Flags& drawn) {
    Reach o;
    o.flags = drawn;
    o.places.insert(m.start);
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto& l : m.locations) {
            if (!o.places.count(l.id)) continue;
            for (const auto& x : l.exits)
                if (AllHold(o.flags, x.needs) && o.places.insert(x.to).second) changed = true;
            for (const auto& e : l.events) {
                if (!AllHold(o.flags, e.needs)) continue;
                const auto before = o.flags.size();
                Earn(o.flags, e.setsFlags);
                changed = changed || o.flags.size() != before;
            }
            for (const auto& n : l.npcs)
                for (const auto& t : n.topics) {
                    if (!AllHold(o.flags, t.needs)) continue;
                    const auto before = o.flags.size();
                    Earn(o.flags, t.setsFlags);
                    changed = changed || o.flags.size() != before;
                }
        }
    }
    return o;
}

// Every combination of the module's draws.
std::vector<Flags> AllDraws(const ModuleDef& m) {
    std::vector<Flags> out{Flags{}};
    for (const auto& d : m.draws) {
        std::vector<Flags> next;
        for (const auto& base : out)
            for (const auto& c : d.choices) {
                Flags f = base;
                f[d.id] = c;
                next.push_back(f);
            }
        out = std::move(next);
    }
    return out;
}

}  // namespace

class ModuleContent : public ::testing::TestWithParam<const char*> {
protected:
    const ModuleDef& M() const {
        const ModuleDef* m = Content::FindModule(GetParam());
        EXPECT_NE(m, nullptr);
        return *m;
    }
};

TEST_P(ModuleContent, EveryPlaceIsConnectedFromTheStart) {
    const ModuleDef& m = M();
    std::set<std::string> seen{m.start};
    std::queue<std::string> q;
    q.push(m.start);
    while (!q.empty()) {
        const LocationDef* l = m.FindLocation(q.front());
        q.pop();
        for (const auto& x : l->exits)
            if (seen.insert(x.to).second) q.push(x.to);
    }
    for (const auto& l : m.locations) EXPECT_TRUE(seen.count(l.id)) << l.id << " cannot be reached from " << m.start;
}

TEST_P(ModuleContent, EveryPlaceCanBeReachedAndLeft) {
    for (const Flags& draw : AllDraws(M())) {
        const Reach o = Closure(M(), draw);
        for (const auto& l : M().locations) {
            EXPECT_TRUE(o.places.count(l.id)) << l.id << " is unreachable once the flags are earned";
            EXPECT_FALSE(l.exits.empty()) << l.id << " is a dead end with no exits";
        }
    }
}

TEST_P(ModuleContent, TheStoryCanBeFinishedWhateverIsDrawn) {
    for (const Flags& draw : AllDraws(M())) {
        const Reach o = Closure(M(), draw);
        std::string which;
        for (const auto& [k, v] : draw) which += k + "=" + v + " ";
        for (const auto& f : M().endWhen) EXPECT_TRUE(o.flags.count(f)) << "end flag '" << f << "' cannot be earned (" << which << ")";
        for (const auto& q : M().quests)
            for (const auto& f : q.completeWhen) EXPECT_TRUE(o.flags.count(f)) << "quest '" << q.id << "' needs '" << f << "', which cannot be earned (" << which << ")";
    }
}

TEST_P(ModuleContent, EveryRequirementNamesAFlagThatExists) {
    const ModuleDef& m = M();
    std::set<std::string> known;
    for (const auto& d : m.draws) known.insert(d.id);
    for (const auto& l : m.locations) {
        for (const auto& e : l.events) known.insert(e.setsFlags.begin(), e.setsFlags.end());
        for (const auto& n : l.npcs)
            for (const auto& t : n.topics) known.insert(t.setsFlags.begin(), t.setsFlags.end());
    }
    auto check = [&](const std::vector<std::string>& needs, const std::string& where) {
        for (const auto& n : needs) EXPECT_TRUE(known.count(ParseCond(n).name)) << where << " requires '" << n << "', which nothing sets (a typo?)";
    };
    for (const auto& l : m.locations) {
        for (const auto& x : l.exits) check(x.needs, l.id + " exit to " + x.to);
        for (const auto& e : l.events) check(e.needs, l.id + "/" + e.id);
        for (const auto& n : l.npcs)
            for (const auto& t : n.topics) check(t.needs, l.id + "/" + n.id + "/" + t.id);
    }
    for (const auto& q : m.quests) check(q.completeWhen, "quest " + q.id);
    check(m.endWhen, "endWhen");
}

TEST_P(ModuleContent, DrawnValuesAreUsedByAnEvent) {
    const ModuleDef& m = M();
    for (const auto& d : m.draws)
        for (const auto& choice : d.choices) {
            bool used = false;
            for (const auto& l : m.locations)
                for (const auto& e : l.events)
                    for (const auto& n : e.needs) {
                        const Cond c = ParseCond(n);
                        used = used || (!c.negate && c.name == d.id && c.hasValue && c.value == choice);
                    }
            EXPECT_TRUE(used) << "draw " << d.id << "=" << choice << " never unlocks anything";
        }
}

TEST_P(ModuleContent, IdsAreUniqueAndTextIsPresent) {
    const ModuleDef& m = M();
    EXPECT_FALSE(m.name.empty());
    EXPECT_FALSE(m.intro.empty());
    EXPECT_FALSE(m.endText.empty());
    std::set<std::string> places, npcs, quests;
    for (const auto& l : m.locations) {
        EXPECT_TRUE(places.insert(l.id).second) << "duplicate place " << l.id;
        EXPECT_FALSE(l.description.empty()) << l.id << " has no description";
        std::set<std::string> events;
        for (const auto& e : l.events) EXPECT_TRUE(events.insert(e.id).second) << l.id << " repeats event " << e.id;
        for (const auto& n : l.npcs) {
            EXPECT_TRUE(npcs.insert(n.id).second) << "duplicate npc " << n.id;
            EXPECT_FALSE(n.topics.empty()) << n.id << " has nothing to say";
            std::set<std::string> topics;
            for (const auto& t : n.topics) EXPECT_TRUE(topics.insert(t.id).second) << n.id << " repeats topic " << t.id;
        }
    }
    for (const auto& q : m.quests) EXPECT_TRUE(quests.insert(q.id).second) << "duplicate quest " << q.id;
}

TEST_P(ModuleContent, FightsHaveSomewhereToStand) {
    const ModuleDef& m = M();
    for (const auto& l : m.locations)
        for (const auto& e : l.events) {
            if (!e.encounter) continue;
            const auto& map = e.map.empty() ? l.map : e.map;
            if (map.empty()) continue;  // the engine lays out a default field
            int party = 0, monsters = 0;
            for (const auto& row : map)
                for (char c : row) {
                    party += c == 'P';
                    monsters += c == 'M';
                }
            EXPECT_GE(party, 4) << l.id << ": the map has " << party << " party squares";
            if (!e.encounter->monsters.empty()) { EXPECT_GE(monsters, 1) << l.id << " has no monster squares"; }
        }
}

TEST_P(ModuleContent, TheAutopilotAlwaysFinishesAndOftenWins) {
    int won = 0;
    for (std::uint64_t seed = 1; seed <= 8; ++seed) {
        GameConfig c;
        c.moduleId = GetParam();
        c.seed = seed;
        c.role = Role::Spectator;
        Game g(c);
        int steps = 0;
        while ((g.GetMode() == Mode::Exploring || g.GetMode() == Mode::Combat) && steps++ < 4000)
            if (!g.AutoStep().ok) break;
        EXPECT_TRUE(g.GetMode() == Mode::Complete || g.GetMode() == Mode::Defeated) << "seed " << seed << " did not finish";
        if (g.GetMode() == Mode::Complete) {
            ++won;
            for (const auto& f : M().endWhen) EXPECT_TRUE(g.HasFlag(f));
        }
    }
    EXPECT_GE(won, 3) << "the autopilot should win at least a third of the time";
}

INSTANTIATE_TEST_SUITE_P(Shipped, ModuleContent, ::testing::Values("ravenmoor", "cyberhood"));

TEST(Modules, EveryShippedModuleIsCovered) {
    EXPECT_EQ(Content::Modules().size(), 2u) << "add the new module's id to INSTANTIATE_TEST_SUITE_P(Shipped, ModuleContent, ...)";
}

// ---------------------------------------------------------------- CyberHood specifics

TEST(CyberHood, HasElevenScenesAndItsOwnCreatures) {
    const ModuleDef* m = Content::FindModule("cyberhood");
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->locations.size(), 11u);
    EXPECT_EQ(m->start, "transit_hub");
    for (const char* id : {"gutter_punk", "scrap_drone", "static_addict", "nanoswarm", "chrome_runner", "corp_sec_officer", "security_droid", "gang_boss", "heavy_mech", "hush_avatar"}) {
        bool found = false;
        for (const auto& mon : Content::Monsters()) found = found || mon.id == id;
        EXPECT_TRUE(found) << id;
    }
}

TEST(CyberHood, ThePlazaCanBeEnteredWithAPassOrOverTheRoofs) {
    const ModuleDef* m = Content::FindModule("cyberhood");
    ASSERT_NE(m, nullptr);
    Flags none{{"override", "canopy"}};
    // without the pass, only the skybridge leads in
    ModuleDef noPass = *m;
    for (auto& l : noPass.locations)
        for (auto& e : l.events) e.setsFlags.erase(std::remove(e.setsFlags.begin(), e.setsFlags.end(), "has_halcyon_pass"), e.setsFlags.end());
    EXPECT_TRUE(Closure(noPass, none).places.count("halcyon_plaza"));
    // without clearing the canopy, only the pass does
    ModuleDef noRoof = *m;
    for (auto& l : noRoof.locations)
        for (auto& e : l.events) e.setsFlags.erase(std::remove(e.setsFlags.begin(), e.setsFlags.end(), "canopy_cleared"), e.setsFlags.end());
    EXPECT_TRUE(Closure(noRoof, none).places.count("halcyon_plaza"));
}

TEST(CyberHood, TheOverrideKeyIsDrawnFromTwoPlaces) {
    std::set<std::string> seen;
    for (std::uint64_t seed = 1; seed <= 40; ++seed) {
        GameConfig c;
        c.moduleId = "cyberhood";
        c.seed = seed;
        c.role = Role::Spectator;
        Game g(c);
        ASSERT_TRUE(g.HasFlag("override"));
        seen.insert(g.FlagValue("override"));
    }
    EXPECT_EQ(seen, (std::set<std::string>{"canopy", "undergrid"}));
}
