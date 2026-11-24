#pragma once
#include <functional>
#include <string>
#include <vector>

namespace dnd {

enum class EventType {
    CombatStart, RoundStart, TurnStart, Move, Attack, Spell, Heal,
    Down, Death, DeathSave, Info, CombatEnd,
    Narration, Scene, Quest, Rest, Travel
};

struct Event {
    EventType type = EventType::Info;
    int actor = -1;
    int target = -1;
    int roll = 0;
    int vs = 0;
    int amount = 0;
    std::string text{};
    int seq = 0;           // assigned by Game; strictly increasing
    std::string flavour{}; // optional prose added by the narrator

    // Set when roll/vs is a d20 test, so clients can explain the result without parsing text.
    std::string check{};     // "attack" (roll = total to hit, vs = AC incl. cover) or "save" (roll = save total, vs = DC); empty otherwise
    int natural = 0;         // the kept d20 face of an attack roll
    std::string mode{};      // "advantage", "disadvantage" or empty
    bool crit = false;       // the attack was a critical hit
    int coverBonus = 0;      // AC (attack) or save bonus (save) the target got from cover
    bool opportunity = false;  // an opportunity attack, taken as a reaction
};

// Synchronous publish/subscribe. Rule code emits; narrators, loggers and tests listen.
class EventBus {
public:
    using Handler = std::function<void(const Event&)>;
    void Subscribe(Handler h) { handlers_.push_back(std::move(h)); }
    void Emit(const Event& e) const {
        for (const auto& h : handlers_) h(e);
    }

private:
    std::vector<Handler> handlers_;
};

}  // namespace dnd
