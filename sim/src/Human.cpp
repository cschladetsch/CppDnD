#include "dnd/Human.hpp"

#include <algorithm>
#include <cctype>
#include <istream>
#include <optional>
#include <ostream>
#include <sstream>

#include "dnd/Actor.hpp"
#include "dnd/Combat.hpp"
#include "dnd/Content.hpp"

namespace dnd {

void HumanController::ShowHelp() const {
    out_ << "  Commands:\n"
            "    m <x> <y>                  move toward a square\n"
            "    a <id> [attack#]           attack target id (attack# optional)\n"
            "    c <spell#|name> <id> [slot]  cast a spell at id\n"
            "    p [id]                     drink a healing potion (self or adjacent ally)\n"
            "    dash | dodge | e           dash, dodge, end turn\n"
            "    map                        redraw the map\n"
            "    swap player|dm|auto        change who you are playing, right now\n"
            "    help\n";
}

void HumanController::ShowTurn(const TurnContext& ctx) const {
    const auto& me = ctx.self;
    const auto& s = me.sheet;
    out_ << '\n' << ctx.combat.RenderMap();
    out_ << "  >> " << me.Name() << ": HP " << s.hp << "/" << s.maxHp << "  Move " << s.economy.movementFt << " ft  ["
         << (s.economy.action ? "Action " : "") << (s.economy.bonusAction ? "Bonus " : "") << (s.economy.reaction ? "Reaction" : "") << "]\n";
    for (std::size_t i = 0; i < s.attacks.size(); ++i) {
        const auto& w = s.attacks[i];
        out_ << "     attack " << i << ": " << w.name << " +" << w.toHit << " " << w.damage.ToString() << " "
             << ToString(w.type) << " (" << w.rangeFt << (w.IsRanged() ? "/" + std::to_string(w.longRangeFt) : "") << " ft)\n";
    }
    for (std::size_t i = 0; i < s.spells.size(); ++i) {
        if (const Spell* sp = Content::FindSpell(s.spells[i]))
            out_ << "     spell  " << i << ": " << sp->name << " (lvl " << sp->level << ", " << sp->rangeFt << " ft"
                 << (sp->bonusAction ? ", bonus" : "") << ")\n";
    }
    bool anySlot = false;
    for (int l = 1; l <= 9; ++l)
        if (s.slotsMax[static_cast<std::size_t>(l)]) {
            out_ << (anySlot ? ", " : "     slots: ") << "L" << l << ":" << s.slots[static_cast<std::size_t>(l)];
            anySlot = true;
        }
    if (anySlot) out_ << '\n';
    if (s.potions) out_ << "     potions: " << s.potions << '\n';
}

Action HumanController::DecideAction(const TurnContext& ctx) {
    const auto& me = ctx.self;
    if (ctx.combat.Round() != lastRound_ || me.id != lastActor_) {
        lastRound_ = ctx.combat.Round();
        lastActor_ = me.id;
        ShowTurn(ctx);
    } else {
        const auto& e = me.sheet.economy;
        out_ << "  >> HP " << me.sheet.hp << "/" << me.sheet.maxHp << "  Move " << e.movementFt << " ft  ["
             << (e.action ? "Action " : "") << (e.bonusAction ? "Bonus " : "") << (e.reaction ? "Reaction" : "") << "]  ('map' to redraw)\n";
    }
    for (;;) {
        out_ << me.Name() << "> " << std::flush;
        std::string line;
        if (!std::getline(in_, line)) {
            out_ << "\n[input closed: handing control to the AI]\n";
            Action a;
            a.type = ActionType::SwapRole;
            a.role = Role::Spectator;
            return a;
        }
        std::istringstream ss(line);
        std::string cmd;
        if (!(ss >> cmd)) continue;
        std::transform(cmd.begin(), cmd.end(), cmd.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

        Action a;
        if (cmd == "e" || cmd == "end") return Action::End();
        if (cmd == "help" || cmd == "?") { ShowHelp(); continue; }
        if (cmd == "map") { out_ << ctx.combat.RenderMap(); continue; }
        if (cmd == "dash") { a.type = ActionType::Dash; return a; }
        if (cmd == "dodge") { a.type = ActionType::Dodge; return a; }
        if (cmd == "m") {
            int x, y;
            if (!(ss >> x >> y)) { out_ << "  usage: m <x> <y>\n"; continue; }
            a.type = ActionType::Move;
            a.dest = Pos{x, y};
            return a;
        }
        if (cmd == "a") {
            int id;
            if (!(ss >> id)) { out_ << "  usage: a <id> [attack#]\n"; continue; }
            a.type = ActionType::Attack;
            a.targetId = id;
            int idx;
            if (ss >> idx) a.attackIndex = idx;
            return a;
        }
        if (cmd == "p") {
            a.type = ActionType::UsePotion;
            int id;
            a.targetId = (ss >> id) ? id : me.id;
            return a;
        }
        if (cmd == "c") {
            std::string which;
            int id;
            if (!(ss >> which >> id)) { out_ << "  usage: c <spell#|name> <id> [slot]\n"; continue; }
            a.type = ActionType::CastSpell;
            a.targetId = id;
            const bool numeric = !which.empty() && std::all_of(which.begin(), which.end(), [](unsigned char ch) { return std::isdigit(ch); });
            if (numeric) {
                const std::size_t n = static_cast<std::size_t>(std::stoul(which));
                if (n >= me.sheet.spells.size()) { out_ << "  no such spell number\n"; continue; }
                a.spellId = me.sheet.spells[n];
            } else {
                a.spellId = which;
            }
            int slot;
            if (ss >> slot) a.slotLevel = slot;
            return a;
        }
        if (cmd == "swap") {
            std::string r;
            ss >> r;
            a.type = ActionType::SwapRole;
            if (r == "player") a.role = Role::Player;
            else if (r == "dm") a.role = Role::DungeonMaster;
            else if (r == "auto") a.role = Role::Spectator;
            else { out_ << "  usage: swap player|dm|auto\n"; continue; }
            return a;
        }
        out_ << "  unknown command (try 'help')\n";
    }
}

}  // namespace dnd
