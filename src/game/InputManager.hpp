#pragma once
#include "../types.h"
#include "Singleton.hpp"

#include <string>
#include <cctype>
#include <cstdlib>

// 1:1 translation of Assets\Scripts\Core\UI\Settings\ControlMapper\InputManager.cs (296 lines,
// pristine decompile v5). NOTE the namespace path: despite living in the ControlMapper folder,
// this is NOT the excluded ControlMapper settings UI (ControlsMenu.cs stays excluded) -- this
// class IS the game's input mapper: it defines `InputAction` and every gameplay script
// (Player/Door/GC/Notebook/Pickup) routes through Singleton<InputManager>::Instance().
// C# -> C++ notes:
//  - Dictionary<InputAction, InputBinding> -> fixed arrays + hasBinding[]: InputAction is a
//    dense 0..Count enum, Unity never serializes Dictionaries (no bake surface), and the code
//    either walks all keys (Save/Load) or does a TryGetValue that the flag reproduces exactly.
//  - Dictionary<KeyCode, SimulatedKeyState> -> array over the KeyCode slot space (types.h
//    kKeySlots): an absent key is the all-default entry; TryGetValue's false path coincides
//    with a defaulted entry's currentFrame == false, so GetKeyState behaves identically.
//  - GetKeyState -> Input::GetKey (HELD state, engine impl) + simulated keys.
//  - Mappings (Dictionary getter) is not translated yet: only the excluded ControlMapper UI
//    reads it; revisit when that batch lands.
//  - The engine's Update sweep currently runs this script at its REGISTRY position (it is
//    runtime-created, so in real play it precedes scene objects created later than it -- Unity
//    makes no cross-GameObject Update-order guarantee either). Consequence only: a consumer in
//    the same registry frame may read the previous frame's refreshed state, a one-frame lag
//    with Unity-identical edge semantics.
namespace bb {

// C# key.ToString() names for the display path (excluded ControlMapper UI's future consumer).
inline std::string KeyCodeName(KeyCode key) {
  switch (key) {
    case KeyCode::A: return "A";
    case KeyCode::D: return "D";
    case KeyCode::S: return "S";
    case KeyCode::W: return "W";
    case KeyCode::Y: return "Y";
    case KeyCode::N: return "N";
    case KeyCode::Space: return "Space";
    case KeyCode::Escape: return "Escape";
    case KeyCode::LeftShift: return "LeftShift";
    case KeyCode::Alpha0: return "Alpha0";
    case KeyCode::Alpha1: return "Alpha1";
    case KeyCode::Alpha2: return "Alpha2";
    case KeyCode::Alpha3: return "Alpha3";
    case KeyCode::Alpha4: return "Alpha4";
    case KeyCode::Alpha5: return "Alpha5";
    case KeyCode::Alpha6: return "Alpha6";
    case KeyCode::Alpha7: return "Alpha7";
    case KeyCode::Alpha8: return "Alpha8";
    case KeyCode::Alpha9: return "Alpha9";
    case KeyCode::Mouse0: return "Mouse0";
    case KeyCode::Mouse1: return "Mouse1";
    case KeyCode::Mouse2: return "Mouse2";
    default: return "";
  }
}

inline std::string InputActionName(InputAction action) {
  switch (action) {
    case InputAction::MoveLeft: return "MoveLeft";
    case InputAction::MoveRight: return "MoveRight";
    case InputAction::MoveForward: return "MoveForward";
    case InputAction::MoveBackward: return "MoveBackward";
    case InputAction::Interact: return "Interact";
    case InputAction::UseItem: return "UseItem";
    case InputAction::Slot0: return "Slot0";
    case InputAction::Slot1: return "Slot1";
    case InputAction::Slot2: return "Slot2";
    case InputAction::Run: return "Run";
    case InputAction::LookBehind: return "LookBehind";
    case InputAction::Jump: return "Jump";
    case InputAction::PauseOrCancel: return "PauseOrCancel";
    default: return "";
  }
}

struct InputBinding {
  KeyCode primaryKey = KeyCode::None;
  KeyCode secondaryKey = KeyCode::None;
  InputBinding() {}
  InputBinding(KeyCode primary, KeyCode secondary = KeyCode::None)
      : primaryKey(primary), secondaryKey(secondary) {}
};

struct SimulatedKeyState {
  bool currentFrame = false;
  bool previousFrame = false;
};

struct InputManager : public Singleton<InputManager> {
  static const char* TypeName() { return "InputManager"; }

  static const int kActionCount = (int)InputAction::Count;
  static const int kCodeSlots = bb::kKeySlots;
  static const char* SavePrefix() { return "InputBinding_"; }

  bool hasBinding[kActionCount];
  InputBinding KeyboardMapping[kActionCount];
  SimulatedKeyState simulatedKeys[kCodeSlots];
  bool currentKeyStates[kActionCount];
  bool previousKeyStates[kActionCount];

  InputManager() {
    for (int i = 0; i < kActionCount; i++) {
      hasBinding[i] = false;
      KeyboardMapping[i] = InputBinding();
      currentKeyStates[i] = false;
      previousKeyStates[i] = false;
    }
    for (int k = 0; k < kCodeSlots; k++) simulatedKeys[k] = SimulatedKeyState();
  }

  // C# Awake: dedupe branch inside base.Awake() (Instance != this -> Destroy(gameObject)), then
  // DontDestroyOnLoad(gameObject); Load();
  virtual void Awake() override {
    Singleton<InputManager>::Awake();
    DontDestroyOnLoad(gameObject);
    Load();
  }

  virtual void Update(float) override {
    // C#: bool[] tempStates = previousKeyStates; previousKeyStates = currentKeyStates;
    //     currentKeyStates = tempStates;  -- the arrays swap ROLES each frame; unbound actions
    // keep their (stale) counters exactly like the C# `continue` skips them.
    bool tempStates[kActionCount];
    for (int i = 0; i < kActionCount; i++) tempStates[i] = previousKeyStates[i];
    for (int i = 0; i < kActionCount; i++) previousKeyStates[i] = currentKeyStates[i];
    for (int i = 0; i < kActionCount; i++) currentKeyStates[i] = tempStates[i];

    UpdateSimulatedKeys();

    for (int i = 0; i < kActionCount; i++) {
      if (!hasBinding[i]) continue;
      const InputBinding& binding = KeyboardMapping[i];
      bool keyDown =
          (binding.primaryKey != KeyCode::None && GetKeyState(binding.primaryKey)) ||
          (binding.secondaryKey != KeyCode::None && GetKeyState(binding.secondaryKey));
      currentKeyStates[i] = keyDown;
    }
  }

  void UpdateSimulatedKeys() {
    for (int k = 0; k < kCodeSlots; k++) {
      SimulatedKeyState& state = simulatedKeys[k];
      if (!state.currentFrame && !state.previousFrame) continue;  // C#: key absent from the Map
      state.previousFrame = state.currentFrame;
      state.currentFrame = false;
      if (!state.currentFrame && !state.previousFrame) {
        simulatedKeys[k] = SimulatedKeyState();  // C#: simulatedKeys.Remove(key)
      }
    }
  }

  void SimulateKey(KeyCode key) {
    if ((int)key < 0 || (int)key >= kCodeSlots) return;
    simulatedKeys[(int)key].currentFrame = true;
  }

  bool GetKeyState(KeyCode key) {
    if ((int)key < 0 || (int)key >= kCodeSlots) return false;
    if (simulatedKeys[(int)key].currentFrame) return true;
    return Input::GetKey(key);
  }

  bool GetActionKey(InputAction action) { return currentKeyStates[(int)action]; }

  bool GetActionKeyDown(InputAction action) {
    return currentKeyStates[(int)action] && !previousKeyStates[(int)action];
  }

  bool GetActionKeyUp(InputAction action) {
    return !currentKeyStates[(int)action] && previousKeyStates[(int)action];
  }

  void ClearPrimaryKey(InputAction action) {
    if ((int)action < 0 || (int)action >= kActionCount) return;
    if (!hasBinding[(int)action]) return;
    KeyboardMapping[(int)action].primaryKey = KeyCode::None;
  }

  void ClearSecondaryKey(InputAction action) {
    if ((int)action < 0 || (int)action >= kActionCount) return;
    if (!hasBinding[(int)action]) return;
    KeyboardMapping[(int)action].secondaryKey = KeyCode::None;
  }

  // C# ConvertInputActionToString: the bound primary key as display text, else the secondary,
  // else the enum name.
  std::string ConvertInputActionToString(InputAction action) {
    if ((int)action < 0 || (int)action >= kActionCount) return "";
    if (!hasBinding[(int)action]) return InputActionName(action);
    const InputBinding& binding = KeyboardMapping[(int)action];
    if (binding.primaryKey != KeyCode::None) return KeyCodeToDisplayString(binding.primaryKey);
    if (binding.secondaryKey != KeyCode::None) return KeyCodeToDisplayString(binding.secondaryKey);
    return InputActionName(action);
  }

  void Modify(InputAction action, KeyCode newKey, KeyCode secondaryKey = KeyCode::None) {
    if ((int)action < 0 || (int)action >= kActionCount) return;
    InputBinding binding;
    if (hasBinding[(int)action]) binding = KeyboardMapping[(int)action];
    if (newKey != KeyCode::None) binding.primaryKey = newKey;
    if (secondaryKey != KeyCode::None) binding.secondaryKey = secondaryKey;
    KeyboardMapping[(int)action] = binding;
    hasBinding[(int)action] = true;
  }

  // C#: foreach pair -> PlayerPrefs "InputBinding_<action>" = "<primary>,<secondary>"; PlayerPrefs.Save().
  void Save() {
    for (int i = 0; i < kActionCount; i++) {
      if (!hasBinding[i]) continue;
      const std::string value = std::to_string((int)KeyboardMapping[i].primaryKey) + "," +
                                std::to_string((int)KeyboardMapping[i].secondaryKey);
      PlayerPrefs::SetString(SavePrefix() + std::to_string(i), value);
    }
    PlayerPrefs::Save();
  }

  void Load() {
    for (int i = 0; i < kActionCount; i++) {  // C#: KeyboardMapping.Clear()
      hasBinding[i] = false;
    }
    for (int i = 0; i < kActionCount; i++) {
      const std::string key = SavePrefix() + std::to_string(i);
      if (PlayerPrefs::HasKey(key)) {
        const std::string raw = PlayerPrefs::GetString(key);
        int primary = 0, secondary = 0;
        if (ParseBindingPair(raw, primary, secondary)) {
          KeyboardMapping[i] = InputBinding((KeyCode)primary, (KeyCode)secondary);
          hasBinding[i] = true;
          continue;
        }
      }
      KeyboardMapping[i] = GetDefaultBinding((InputAction)i);  // C# fallback path
      hasBinding[i] = true;
    }
  }

  void SetDefaults() {
    for (int i = 0; i < kActionCount; i++) {
      KeyboardMapping[i] = GetDefaultBinding((InputAction)i);
      hasBinding[i] = true;
    }
    Save();
  }

  // C# KeyCodeToDisplayString: ESC / "Left/Right/Middle Mouse Button" / digits / "" for None;
  // anything else prints its enum name (W, S, Space, LeftShift, ...).
  static std::string KeyCodeToDisplayString(KeyCode key) {
    switch (key) {
      case KeyCode::Escape: return "ESC";
      case KeyCode::Mouse0: return "Left Mouse Button";
      case KeyCode::Mouse1: return "Right Mouse Button";
      case KeyCode::Mouse2: return "Middle Mouse Button";
      case KeyCode::Alpha0: return "0";
      case KeyCode::Alpha1: return "1";
      case KeyCode::Alpha2: return "2";
      case KeyCode::Alpha3: return "3";
      case KeyCode::Alpha4: return "4";
      case KeyCode::Alpha5: return "5";
      case KeyCode::Alpha6: return "6";
      case KeyCode::Alpha7: return "7";
      case KeyCode::Alpha8: return "8";
      case KeyCode::Alpha9: return "9";
      case KeyCode::None: return "";
      default: return KeyCodeName(key);
    }
  }

 private:
  // C# int.TryParse on ALL parts of Split(',') with parts.Length == 2 required.
  static bool ParseBindingPair(const std::string& raw, int& primary, int& secondary) {
    const size_t comma = raw.find(',');
    if (comma == std::string::npos) return false;
    if (raw.find(',', comma + 1) != std::string::npos) return false;
    return TryParse(raw.substr(0, comma), primary) &&
           TryParse(raw.substr(comma + 1), secondary);
  }

  static bool TryParse(const std::string& s, int& out) {
    if (s.empty()) return false;
    size_t i = (s[0] == '+' || s[0] == '-') ? 1 : 0;
    if (i >= s.size()) return false;
    for (size_t j = i; j < s.size(); j++) {
      if (!isdigit((unsigned char)s[j])) return false;
    }
    const long v = strtol(s.c_str(), 0, 10);
    if (v < -2147483647L - 1 || v > 2147483647L) return false;
    out = (int)v;
    return true;
  }

  InputBinding GetDefaultBinding(InputAction action) {
    switch (action) {
      case InputAction::MoveLeft: return InputBinding(KeyCode::A);
      case InputAction::MoveRight: return InputBinding(KeyCode::D);
      case InputAction::MoveForward: return InputBinding(KeyCode::W);
      case InputAction::MoveBackward: return InputBinding(KeyCode::S);
      case InputAction::Interact: return InputBinding(KeyCode::None, KeyCode::Mouse0);
      case InputAction::UseItem: return InputBinding(KeyCode::None, KeyCode::Mouse1);
      case InputAction::Slot0: return InputBinding(KeyCode::Alpha1);
      case InputAction::Slot1: return InputBinding(KeyCode::Alpha2);
      case InputAction::Slot2: return InputBinding(KeyCode::Alpha3);
      case InputAction::Run: return InputBinding(KeyCode::LeftShift);
      case InputAction::LookBehind: return InputBinding(KeyCode::Space);
      case InputAction::Jump: return InputBinding(KeyCode::Space, KeyCode::None);
      case InputAction::PauseOrCancel: return InputBinding(KeyCode::Escape);
      default: return InputBinding(KeyCode::None);
    }
  }
};

} // namespace bb
