# include

Public headers, all under `dnd/`, namespace `dnd`. Each has a matching `.cpp` in [../src](../src/README.md) except the header-only ones noted.

| Header | Holds |
|---|---|
| `Types.hpp` | abilities, damage types, conditions, teams, roles, `Pos`, `WeaponAttack`, `AttackRider` |
| `Dice.hpp` | `DiceExpr` (`2d6+3`), advantage and disadvantage, the serializable `Rng`, and `DiceRoller` |
| `Util.hpp` | small helpers (header-only) |
| `Events.hpp` | `Event` and the synchronous `EventBus` that rules emit to and narrators listen on (header-only) |
| `Grid.hpp` | `TacticalGrid`: 5 ft squares, terrain, cover, line of sight, movement cost |
| `Actor.hpp` | `GameActor`: a sheet plus position, life state, effects and its controller |
| `Controller.hpp` | `Action`, `TurnContext`, `IController`, and `RemoteController` (never answers by itself, so the server never blocks) |
| `Combat.hpp` | initiative, action economy and rules resolution; `Advance` / `Submit` / `AwaitingActor` |
| `AI.hpp` | the utility-scored tactical `AIController` used for monsters and AI party members |
| `Human.hpp` | the terminal `HumanController` |
| `DM.hpp` | `DungeonMasterEngine`: builds encounters from an XP budget |
| `Content.hpp` | class, weapon, spell, monster, character and module definitions, and the loader |
| `Game.hpp` | `Game`: the session (exploration, clock, rests, quests, save and load) |
| `Json.hpp` | lossless JSON for saves and the API, and `CombatView` for clients |
| `Scene.hpp` | `Camera`, `BuildScene` (the scene a camera sees, from `Game::View()`), `Project`, `CameraWords`, `DescribeView` |
| `Images.hpp` | `ImageConfig` (images.json), `ArtSubjects` (what can have a picture), image clients (AUTOMATIC1111 and OpenAI-style), and `ArtService`, the background queue that draws into the art folder using a model from the model store |
| `History.hpp` | `History`: private per-pair records (x-y.json, from x's side) and the group record, with JSON dump, restore and file output |
| `Agents.hpp` | `AgentsConfig` (agents.json), `Agent` (own model, persona, memory), `AgentHub` (JSON messages between agents), `AgentTurnController`, and `AgentTable` (one per game: DM, enemy types, players, NPCs, council, model switching) |
| `Llm.hpp` | model store lookup, `HttpLlmClient`, narrator, `LlmDmController`, and `Say` (plain English) |

`Game.hpp` and `Combat.hpp` are the two to read first.
