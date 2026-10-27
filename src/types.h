// types.h  -  data model matching data/scene-*.graph.json (Phase 0 output),
// plus the constants recovered from ProjectSettings (tags, layers, input actions).
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "bbmath.h"

namespace bb {

using ObjectId = int64_t;   // Unity fileID
using Guid = std::string;   // 32-hex asset identifier

struct ObjectRef { // serialized { fileID, guid?, type? }
  ObjectId fileID = 0;        // 0 = null reference
  Guid guid;                  // empty = intra-file reference
  int type = 0;
  bool isNull() const { return fileID == 0; }
  bool isExternal() const { return !guid.empty(); }
};

struct GameObjectData {
  std::string name;
  int layer = 0;
  bool active = true;
  std::string tag = "Untagged";
  unsigned staticFlags = 0;
  std::vector<ObjectId> comps;
};

struct ComponentData {
  std::string typeName; // Unity class name verbatim
  ObjectId go = 0;      // owning GameObject
};

// ---- layers (TagManager.asset; builtin 5 = UI) ----
enum {
  LAYER_DEFAULT = 0, LAYER_UI = 5,
  LAYER_NON_PLAYER = 8, LAYER_PLAYER = 9, LAYER_TRIGGER = 10, LAYER_PLAYER_TRIGGER = 11,
};

// ---- custom tags ----
const char* const kTags[] = {
  "Joe", "Item", "NPC", "Notebook", "SwingingDoor", "Door", "BSODA", "YCTP_Button", "Button",
};

// ---- stock input actions (InputManager.asset)  -  testbed keyboard defaults & touch action names ----
enum class Action : int {
  Strafe = 0,     // d/a, alt right/left
  Forward,        // w/s, alt up/down
  Run,            // left shift
  LookBehind,     // space (stock has Jump also on space; touch layer re-maps)
  Pause,          // escape, alt p
  Jump,           // space
  MouseX, MouseY, ScrollWheel,
  JoystickX, JoystickY,   // Horizontal/Vertical, type 2
  Submit,         // enter/return
  Cancel,         // escape
  Interact,       // LMB
  Item,           // RMB
  Count,
};

// ---- Rewired actions (Core\UI\Settings\ControlMapper\InputManager.cs; enum verbatim) ----
// Unity saves per-action bindings to PlayerPrefs as "InputBinding_<n>" -- the INTEGER key is
// part of the save format, so the enum values MUST match the C# order exactly (they do now;
// an earlier port-internal ordering was re-based before any real save existed). Phase 2 binds
// these to SDL keys (Windows testbed) / the native touch layer (iOS).
enum class InputAction : int {
  MoveLeft = 0,
  MoveRight,
  MoveForward,
  MoveBackward,
  Interact,
  UseItem,
  Slot0,
  Slot1,
  Slot2,
  Run,
  LookBehind,
  Jump,
  PauseOrCancel,
  Count,
};

// ---- KeyCode (Unity enum values VERBATIM; grown as script batches reference more keys) ----
// The integer values are load-bearing: PlayerPrefs stores binding KeyCodes as ints (Save/Load),
// and Unity's None is 0, so a compact re-numbering would collide with it (the old port had Y=0,
// which IS Unity's None). Values below are Unity's real assignments.
enum class KeyCode : int {
  None = 0,
  Backspace = 8,
  Return = 13,
  Space = 32,
  Escape = 27,
  A = 97,
  D = 100,
  N = 110,
  S = 115,
  W = 119,
  Y = 121,
  Minus = 45,
  Alpha0 = 256,
  Alpha1, Alpha2, Alpha3, Alpha4, Alpha5, Alpha6, Alpha7, Alpha8, Alpha9,  // ...265
  Mouse0 = 323,
  Mouse1,
  Mouse2,   // Unity: Mouse0..Mouse2 = 323..325
  LeftShift = 304,
};
// Key-slot count for the engine's per-key edge/held arrays. Unity KeyCode values reach Mouse2
// (325), so 512 slots leave headroom for later batches without renumbering anything.
const int kKeySlots = 512;

// ---- Unity QueryTriggerInteraction (raycast trigger-hit policy) ----
enum class QueryTriggerInteraction : int {
  UseGlobal = 0,
  Ignore = 1,
  Collide = 2,
};

} // namespace bb
