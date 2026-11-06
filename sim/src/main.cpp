#include <cstdlib>
#include <iostream>
#include <random>
#include <string>

#include "dnd/Session.hpp"

namespace {
void Usage() {
    std::cout <<
        "dnd_sim - single-player D&D 5e tactical simulator\n"
        "  --role player|dm|auto     who you are (default: auto)\n"
        "  --hero <name>             hero to control in player mode (default: Thorin)\n"
        "  --difficulty easy|medium|hard|deadly   (default: medium)\n"
        "  --seed <n>                reproducible run\n"
        "  --quiet                   no narration\n"
        "In a prompt, type 'help'. 'swap player|dm|auto' changes your role mid-fight.\n";
}
}  // namespace

int main(int argc, char** argv) {
    using namespace dnd;
    Role role = Role::Spectator;
    Difficulty diff = Difficulty::Medium;
    std::string hero = "Thorin";
    std::uint64_t seed = std::random_device{}();
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--role") {
            const std::string r = next();
            if (r == "player") role = Role::Player;
            else if (r == "dm") role = Role::DungeonMaster;
            else if (r == "auto") role = Role::Spectator;
            else { Usage(); return 2; }
        } else if (a == "--hero") hero = next();
        else if (a == "--difficulty") {
            const auto d = ParseDifficulty(next());
            if (!d) { Usage(); return 2; }
            diff = *d;
        } else if (a == "--seed") seed = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--quiet") quiet = true;
        else { Usage(); return a == "--help" || a == "-h" ? 0 : 2; }
    }

    std::cout << "seed " << seed << '\n';
    Session session(std::cin, std::cout, seed);
    session.SetHero(hero);
    session.SetQuiet(quiet);
    session.SetRole(role);
    const CombatResult r = session.RunEncounter(diff);
    if (quiet) std::cout << (r.outcome == Outcome::Victory ? "Victory" : r.outcome == Outcome::Defeat ? "Defeat" : "Timeout")
                         << " in " << r.rounds << " rounds\n";
    return 0;
}
