#pragma once
#include "../eng.h"
#include "CursorControllerScript.hpp" // cc.UnlockCursor / LockCursor below

// 1:1 translation of Assets\Scripts\Core\UI\UIController.cs (93 lines; the uGUI focus router that
// swaps mouse-deselect vs joystick-select behavior per menu).
// - C# get-only property `usingJoystick` (always false) -> const method; both joystick branches
//   are dead code in the C# too, kept verbatim.
// - EventSystem / Selectable added to eng.h (selection state Phase 2).
// - `& usingJoystick` chains -> `&&` (side-effect-free).
// - firstButton.OnSelect(null) -> OnSelect(0).
// - OnEnable is a Unity message fired per enable; reselects the dummy button.
namespace bb {

struct UIController : public Component {
  virtual void Start() {
    if (unlockOnStart && !joystickEnabled)
    {
      cc->UnlockCursor();
    }
  }

  virtual void OnEnable() {
    dummyButtonPC->Select();
    UpdateControllerType();
  }

  virtual void Update(float dt) {
    UpdateControllerType();
  }

  void SwitchMenu()
  {
    SelectDummy();
    UpdateControllerType();
  }

  void UpdateControllerType() // private (C#)
  {
    if (!joystickEnabled && usingJoystick())
    {
      joystickEnabled = true;
      if (controlMouse)
      {
        cc->LockCursor();
      }
    }
    else if (joystickEnabled && !usingJoystick())
    {
      joystickEnabled = false;
      if (controlMouse)
      {
        cc->UnlockCursor();
      }
    }
    UIUpdate();
  }

  void UIUpdate() // private (C#)
  {
    if (uiControlEnabled)
    {
      if (usingJoystick())
      {
        if (EventSystem::current()->currentSelectedGameObject()->tag != buttonTag && firstButton != 0)
        {
          firstButton->Select();
          firstButton->OnSelect(0);
        }
      }
      else
      {
        SelectDummy();
      }
    }
  }

  void EnableControl()
  {
    uiControlEnabled = true;
  }

  void DisableControl()
  {
    uiControlEnabled = false;
  }

  void SelectDummy() // private (C#)
  {
    dummyButtonPC->Select();
  }

  bool usingJoystick() const // C# get-only property -> method
  {
    return false;
  }

  // ============ serialized fields (C# field-block order) ============
  CursorControllerScript* cc = 0;
  bool joystickEnabled = false;        // private (C#)
  bool controlMouse = false;
  bool unlockOnStart = false;
  bool uiControlEnabled = false;
  Selectable* firstButton = 0;
  Selectable* dummyButtonPC = 0;
  Selectable* dummyButtonElse = 0;
  std::string buttonTag;
};

} // namespace bb
