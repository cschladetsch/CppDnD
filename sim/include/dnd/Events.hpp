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
    std::string text;
    int seq = 0;           // assigned by Game; strictly increasing
    std::string flavour;   // optional prose added by the narrator
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
