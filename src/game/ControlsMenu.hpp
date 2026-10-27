#pragma once
#include "../eng.h"
#include "Singleton.hpp"
#include "InputManager.hpp"
#include "KeyCodeNames.hpp"

// 1:1 translation of Assets\Scripts\Core\UI\Settings\ControlMapper\ControlsMenu.cs (295 lines) --
// the controls page's rebinding menu. Its public methods are exactly the 35 inspector-wired
// persistent calls the loader used to report as undispatched; game-actions.cpp wires them.
//
// Translation notes (fidelity):
// - InputManager.KeyboardMapping is an array indexed by InputAction here (C#: a Dictionary), so
//   TryGetValue is a bounds check.
// - allKeyCodes (Enum.GetValues(typeof(KeyCode))) is the genuine game's KeyCode table
//   (KeyCodeNames.hpp, read from UnityEngine.CoreModule.dll); key.ToString() is unityKeyCodeName.
// - RebindCoroutine is a per-frame state machine in Update, because the engine's Coroutine has no
//   StopCoroutine and ConfirmRebind/CancelRebind stop it. Same steps, same frames: the first tick
//   runs inside ConfirmRebind (Unity enumerates synchronously), waits while any key is held, yields
//   one frame, then counts down on unscaled time until a key is pressed or 5 s pass.
// - Input.anyKeyDown is not read: the engine's latch consumes on read; the per-key GetKeyDown scan
//   it guards gives the same answer.
namespace bb {

struct ControlsMenu : public Component {
  virtual void Start() override {
    inputMan = Singleton<InputManager>::Instance();
    if (inputMan != 0)
    {
      UpdateAllButtonTexts();
    }
  }

  virtual void OnEnable() override {
    if (inputMan == 0)
    {
      inputMan = Singleton<InputManager>::Instance();
    }
    if (inputMan == 0)
    {
      Debug::Log("ControlsMenu: InputManager not found! Is it in the scene?");
      return;
    }
    UpdateAllButtonTexts();
  }

  virtual void OnDisable() override {
    if (inputMan) inputMan->Save();   // C# derefs unguarded; a disable before Start has no manager
  }

  void UpdateAllButtonTexts() {
    for (int i = 0; i < kTexts; i++)
    {
      if (i < InputManager::kActionCount)
      {
        const InputBinding& binding = inputMan->KeyboardMapping[i];
        if (buttonTexts[i]) buttonTexts[i]->SetText(IsMouseKey(binding.primaryKey) ? FormatMouseText(binding.primaryKey) : FormatKeyText(binding.primaryKey));
        if (secondaryButtonTexts[i]) secondaryButtonTexts[i]->SetText(IsMouseKey(binding.secondaryKey) ? FormatMouseText(binding.secondaryKey) : FormatKeyText(binding.secondaryKey));
      }
    }
  }

  std::string FormatKeyText(KeyCode key) {
    switch (key)
    {
      case KeyCode::Escape: return "ESC";
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
      default: return unityKeyCodeName((int)key);
    }
  }

  std::string FormatMouseText(KeyCode mouseKey) {
    switch (mouseKey)
    {
      case KeyCode::Mouse0: return "Left Mouse Button";
      case KeyCode::Mouse1: return "Right Mouse Button";
      case KeyCode::Mouse2: return "Middle Mouse Button";
      default: return unityKeyCodeName((int)mouseKey);
    }
  }

  void StartRebindKeyboard(int actionIndex) {
    if ((panel && panel->activeSelf) || (confirmationPanel && confirmationPanel->activeSelf))
    {
      return;
    }
    if (actionIndex < InputManager::kActionCount)
    {
      currentActionIndex = actionIndex;
      isBindingMouseInput = false;
      confirmationPanel->SetActive(true);
      SetButtonsInteractable(false);
      confirmText->SetText(labelText(currentActionIndex) + "\n\n" + (buttonTexts[currentActionIndex] ? buttonTexts[currentActionIndex]->GetText() : std::string()));
    }
  }

  void StartRebindMouse(int actionIndex) {
    if ((panel && panel->activeSelf) || (confirmationPanel && confirmationPanel->activeSelf))
    {
      return;
    }
    if (actionIndex < InputManager::kActionCount)
    {
      currentActionIndex = actionIndex;
      isBindingMouseInput = true;
      confirmationPanel->SetActive(true);
      SetButtonsInteractable(false);
      confirmText->SetText(labelText(currentActionIndex) + "\n\n" + (secondaryButtonTexts[currentActionIndex] ? secondaryButtonTexts[currentActionIndex]->GetText() : std::string()));
    }
  }

  void ShowResetConfirmation() {
    resetConfirmationPanel->SetActive(true);
    SetButtonsInteractable(false);
  }

  void ResetKey() {
    if (!isBindingMouseInput)
    {
      inputMan->ClearPrimaryKey((InputAction)currentActionIndex);
    }
    else
    {
      inputMan->ClearSecondaryKey((InputAction)currentActionIndex);
    }
    inputMan->Save();
    UpdateAllButtonTexts();
    CancelRebind();
  }

  void ConfirmRebind() {
    confirmationPanel->SetActive(false);
    panel->SetActive(true);
    rebindPhase = 0;                    // StopCoroutine(rebindCoroutine)
    // StartCoroutine(RebindCoroutine(5f)): the first MoveNext runs now
    remainingTime = 5.0f;
    actionName = labelText(currentActionIndex);
    setCounterText();
    rebindPhase = Input::anyKey() ? 1 : 2;   // 1: while (Input.anyKey) yield; 2: the yield after it
  }

  void ConfirmReset() {
    resetConfirmationPanel->SetActive(false);
    ResetToDefaults();
    SetButtonsInteractable(true);
  }

  void CancelRebind() {
    confirmationPanel->SetActive(false);
    rebindPhase = 0;                    // StopCoroutine(rebindCoroutine)
    SetButtonsInteractable(true);
  }

  void CancelReset() {
    resetConfirmationPanel->SetActive(false);
    SetButtonsInteractable(true);
  }

  // RebindCoroutine, one frame per Update (see the header note)
  virtual void Update(float dt) override {
    (void)dt;
    if (rebindPhase == 1)               // while (Input.anyKey) { yield return null; }
    {
      if (!Input::anyKey()) rebindPhase = 2;   // ...loop exits; this frame's MoveNext then yields once
      return;
    }
    if (rebindPhase == 2)               // the frame after `yield return null`: the countdown starts
    {
      rebindPhase = 3;
    }
    if (rebindPhase == 3)
    {
      if (remainingTime > 0.0f)
      {
        setCounterText();
        KeyCode pressedKey = KeyCode::None;
        if (GetPressedInput(pressedKey))
        {
          if (isBindingMouseInput && IsMouseKey(pressedKey))
          {
            inputMan->Modify((InputAction)currentActionIndex, KeyCode::None, pressedKey);
          }
          else if (!isBindingMouseInput && !IsMouseKey(pressedKey))
          {
            inputMan->Modify((InputAction)currentActionIndex, pressedKey);
          }
          remainingTime = 0.01f;
        }
        remainingTime -= Time::unscaledDeltaTime();
        return;                         // yield return null
      }
      panel->SetActive(false);
      rebindPhase = 0;
      isBindingMouseInput = false;
      SetButtonsInteractable(true);
      inputMan->Save();
      UpdateAllButtonTexts();
    }
  }

  bool GetPressedInput(KeyCode& pressedKey) {
    pressedKey = KeyCode::None;
    if (isBindingMouseInput)
    {
      for (int i = 0; i < 7; i++)
      {
        if (Input::GetMouseButtonDown(i))
        {
          pressedKey = (KeyCode)((int)KeyCode::Mouse0 + i);
          return true;
        }
      }
    }
    else
    {
      for (int k = 1; k < kKeySlots; k++)  // Enum.GetValues(KeyCode): ascending by value
      {
        if (unityKeyCodeName(k)[0] == 0) continue;
        if (Input::GetKeyDown((KeyCode)k) && !IsMouseKey((KeyCode)k))
        {
          pressedKey = (KeyCode)k;
          return true;
        }
      }
    }
    return false;
  }

  bool IsMouseKey(KeyCode key) {
    const int k = (int)key, m0 = (int)KeyCode::Mouse0;
    return k >= m0 && k <= m0 + 4;    // Mouse0..Mouse4
  }

  void SetButtonsInteractable(bool state) {
    for (int i = 0; i < kButtons; i++)
    {
      if (allButtons[i]) allButtons[i]->interactable = state;
    }
  }

  void ResetToDefaults() {
    SetButtonsInteractable(true);
    inputMan->SetDefaults();
    UpdateAllButtonTexts();
  }

  // ============ serialized fields (C# field-block order) ============
  static const int kButtons = 26;     // MainMenu.unity serialises 26 allButtons
  static const int kTexts = 13;       // ...and 13 of each text array
  Button* allButtons[kButtons] = {0};
  UiText* buttonTexts[kTexts] = {0};
  UiText* secondaryButtonTexts[kTexts] = {0};
  UiText* labelTexts[kTexts] = {0};
  GameObject* panel = 0;
  GameObject* confirmationPanel = 0;
  GameObject* resetConfirmationPanel = 0;
  UiText* counterText = 0;
  UiText* confirmText = 0;

  int currentActionIndex = 0;          // private (C#)
  bool isBindingMouseInput = false;    // private (C#)
  InputManager* inputMan = 0;          // private (C#)
  int rebindPhase = 0;                 // RebindCoroutine state: 0 not running
  float remainingTime = 0.0f;
  std::string actionName;

 private:
  std::string labelText(int i) const { return (i >= 0 && i < kTexts && labelTexts[i]) ? labelTexts[i]->GetText() : std::string(); }
  void setCounterText() {
    if (!counterText) return;
    counterText->SetText("<size=16>" + actionName + "<size=21>\nPress a key to assign it to " + actionName +
                         "\n\n\n\n\n\n" + std::to_string(Mathf::CeilToInt(remainingTime)));
  }
};

} // namespace bb
