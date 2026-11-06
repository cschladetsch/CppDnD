# Single-Player C++ Dungeons & Dragons (5e) Simulator Architecture

## 1. System Overview & Core Philosophy

The goal of this project is to create an extensible, single-player D&D 5e engine in C++ that supports seamless switching between playing as a **Party Member (Player Character)** and acting as the **Dungeon Master (DM)**. 

To achieve this, the architecture relies on:
* **Controller Pattern (Decoupled Intelligence):** Game entities (`Character`, `Monster`, `DM Engine`) do not contain hardcoded player input or AI decision logic. Instead, they are driven by `IController` interfaces that can be hot-swapped between `HumanController` and `AIController` at runtime.
* **Event-Driven Combat & Narrative Engine:** Game actions are processed through an event dispatcher, allowing easy logging, rule validation, and automated AI evaluation.
* **Deterministic Rules Engine:** Standardized d20 mechanics, advantage/disadvantage calculation, action economy management, and 2D grid tactical mechanics.

---

## 2. Core Architecture & Component Diagram

```
                              +--------------------+
                              |  Input Manager     |
                              |  (CLI / UI Input)  |
                              +---------+----------+
                                        |
                                        v
                              +--------------------+
                              |  Role Controller   |
                              |  (Human vs AI)     |
                              +----+----------+----+
                                   |          |
         +-------------------------+          +-------------------------+
         |                                                              |
         v                                                              v
+------------------+                                          +-------------------+
|  DM Subsystem    |                                          | Party Subsystem   |
| (Narrative/Encs) |                                          | (Tactical AI)     |
+--------+---------+                                          +---------+---------+
         |                                                              |
         +--------------------------+-----------------------------------+
                                    |
                                    v
                       +---------------------------+
                       |   Core Rules Engine       |
                       | - Dice & Target Checks    |
                       | - Tactical 2D Grid        |
                       | - Initiative Queue        |
                       | - Action Economy Rules    |
                       +---------------------------+
```

---

## 3. Class Structure & Hierarchy

### 3.1. Controller Interface (Polymorphic Role Swapping)
The primary mechanism allowing a user to alternate between a Party Member and the DM.

```
                    +-------------------+
                    |   IController     |
                    | (Abstract Interface)
                    +---------+---------+
                              |
              +---------------+---------------+
              |                               |
              v                               v
    +-------------------+           +-------------------+
    |  HumanController  |           |   AIController    |
    | (CLI / UI Prompts)|           | (Utility / BT Logic)
    +-------------------+           +-------------------+
```

### 3.2. Primary Data Model Classes

1. **`DiceRoller` (Utility):**
   * Static/thread-safe RNG implementation.
   * Handles `d4`, `d6`, `d8`, `d10`, `d12`, `d20`, `d100`.
   * Evaluates advantage, disadvantage, critical hits (natural 20), and critical fails (natural 1).

2. **`CharacterSheet` (Data Component):**
   * Core Stats: Strength, Dexterity, Constitution, Intelligence, Wisdom, Charisma.
   * Modifiers, Armor Class (AC), Proficiency Bonus, Speed, Max HP, Current HP, Temp HP.
   * Action Economy Tracker: Action, Bonus Action, Reaction, Movement Remaining.
   * Spell Slots & Inventory.

3. **`GameActor` (Entity):**
   * Represents both Player Characters (PCs) and Non-Player Characters (NPCs / Monsters).
   * Holds a `CharacterSheet`, spatial coordinates `(x, y)`, and a `std::unique_ptr<IController>`.

4. **`TacticalGrid` (Spatial System):**
   * 2D matrix representing 5-foot squares.
   * Line of sight determination (Bresenham's Line Algorithm).
   * Cover calculations (Half cover [+2 AC], Three-Quarters cover [+5 AC], Total cover).
   * Movement cost calculator (Normal vs. Difficult Terrain).

5. **`DungeonMasterEngine` (DM Manager):**
   * Encounters generator (calculating CR budgets based on party level).
   * Narrative state generator (Template/State-based descriptors for combat events).
   * Holds its own `IController` (can be AI-driven for auto-DM mode or Human-driven when the player assumes the DM role).

---

## 4. Implementation Phasing & Roadmap

```
  +-------------------------------------------------------------------------+
  | Phase 1: Core Mechanics Engine                                          |
  |   - Dice rolling math & statistics engine                               |
  |   - CharacterSheet structure & d20 check validation logic               |
  |   - Action economy & combat turn state machine                          |
  +-------------------------------------------------------------------------+
                                     |
                                     v
  +-------------------------------------------------------------------------+
  | Phase 2: Tactical Grid & Spatial Combat                                 |
  |   - 2D grid matrix implementation (5ft per cell)                        |
  |   - Distance calculation, movement tracking, & line of sight            |
  |   - Initiative queue and basic combat manager loop                      |
  +-------------------------------------------------------------------------+
                                     |
                                     v
  +-------------------------------------------------------------------------+
  | Phase 3: AI Engine & Controller Architecture                            |
  |   - Implement polymorphic IController base interface                    |
  |   - Build AI Party Member utility trees (Healing, Tanking, Casting)    |
  |   - Build AI DM Engine (CR calculation, encounter pacing)               |
  +-------------------------------------------------------------------------+
                                     |
                                     v
  +-------------------------------------------------------------------------+
  | Phase 4: Dynamic Role-Swapping & Game Loop                              |
  |   - Runtime swap mechanism between HumanController and AIController    |
  |   - Command parser for PC and DM actions                                |
  |   - Integrated game loop testing                                        |
  +-------------------------------------------------------------------------+
```

---

## 5. Tactical AI & Decision Trees

### 5.1. Simulated Party Member Utility AI
Party members evaluate actions each turn using a priority-score matrix:

1. **Survival Check:**
   * If `Current HP < 25%` and healing potion/spell is available $\rightarrow$ Execute Heal self.
2. **Support Check:**
   * If an ally is at `0 HP` and character has `Healing Word`/`Cure Wounds` $\rightarrow$ Execute Revival Heal.
3. **Target Selection (Offense):**
   * Evaluate reachable enemies based on movement speed.
   * Target selection score formula:
     $$\text{Target Score} = \frac{\text{Enemy Max HP} - \text{Enemy Current HP}}{\text{Distance to Enemy} + 1} + \text{Threat Level Modifier}$$
4. **Action Execution:**
   * Martial classes: Close distance $\rightarrow$ Attack.
   * Spellcasters: Evaluate cluster density for AoE spells $\rightarrow$ Cast AoE or Cantrip.

### 5.2. Simulated Dungeon Master Logic
* **Encounter Generator:** Calculates total party XP threshold for standard encounter difficulty (*Easy*, *Medium*, *Hard*, *Deadly*).
* **Monster Combat Logic:** Runs basic tactical AI focused on creature intelligence (`INT < 6` attacks closest target; `INT >= 10` targets spellcasters/squishy targets).
* **Narrative Generation:** Emits detailed battle events (e.g., `"Thorin swings his Warhammer at Goblin 1: Hits [Roll 18 vs AC 13] for 8 Bludgeoning damage!"`).

---

## 6. Key C++ System Prototype

```cpp
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// Core Action Types
enum class ActionType { Move, Attack, CastSpell, PassTurn };

struct Action {
    ActionType type;
    std::string targetId;
    int targetX;
    int targetY;
};

// Abstract Controller Interface for Polymorphic Roles
class IController {
public:
    virtual ~IController() = default;
    virtual Action DecideAction(const std::string& entityId) = 0;
};

// Autonomous AI Decision Maker
class AIController : public IController {
public:
    Action DecideAction(const std::string& entityId) override {
        std::cout << "[AI Controller] Evaluating tactical matrix for " << entityId << "...\n";
        return Action{ ActionType::Attack, "Goblin_1", 0, 0 };
    }
};

// Interactive Human Controller
class HumanController : public IController {
public:
    Action DecideAction(const std::string& entityId) override {
        std::cout << "[Human Controller] Prompting input for " << entityId << ":\n";
        std::cout << "  1. Attack\n  2. Pass\nChoice: ";
        int choice = 1; // Simulated selection
        if (choice == 1) {
            return Action{ ActionType::Attack, "Enemy_Boss", 0, 0 };
        }
        return Action{ ActionType::PassTurn, "", 0, 0 };
    }
};

// Unified Actor Class representing PCs and Monsters
class GameActor {
public:
    std::string name;
    int hp;
    int maxHp;
    int ac;
    std::unique_ptr<IController> controller;

    GameActor(std::string actorName, int health, int armor, std::unique_ptr<IController> ctrl)
        : name(std::move(actorName)), hp(health), maxHp(health), ac(armor), controller(std::move(ctrl)) {}

    void SetController(std::unique_ptr<IController> newCtrl) {
        controller = std::move(newCtrl);
    }

    void ExecuteTurn() {
        if (!controller) return;
        Action action = controller->DecideAction(name);
        std::cout << name << " executed action type: " << static_cast<int>(action.type) << "\n";
    }
};

// Dungeon Master Subsystem
class DungeonMasterSystem {
private:
    std::unique_ptr<IController> dmController;

public:
    DungeonMasterSystem(bool isHumanDM) {
        SetHumanDM(isHumanDM);
    }

    void SetHumanDM(bool isHumanDM) {
        if (isHumanDM) {
            dmController = std::make_unique<HumanController>();
        } else {
            dmController = std::make_unique<AIController>();
        }
    }

    void ProcessDMTurn() {
        std::cout << "\n--- Dungeon Master Turn Phase ---\n";
        Action dmAction = dmController->DecideAction("DungeonMaster");
        std::cout << "DM action resolved.\n";
    }
};
```