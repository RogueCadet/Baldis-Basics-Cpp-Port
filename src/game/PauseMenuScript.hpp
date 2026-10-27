#pragma once
#include "../eng.h"
#include "GameControllerScript.hpp" // gc.mouseLocked / LockMouse / UnlockMouse below

// 1:1 translation of Assets\Scripts\Core\UI\PauseMenuScript.cs (27 lines; while paused the game
// keeps cursor focus, unpaused it releases it). C# get-only property `usingJoystick` (always
// false) -> const method; the dead EventSystem arm kept verbatim (convention: side-effect-free
// `&` operands -> `&&`, so the dead arm still never runs the mouse calls).
namespace bb {

struct PauseMenuScript : public Component {
  virtual void Update(float dt) {
    if (usingJoystick() && EventSystem::current()->currentSelectedGameObject() == 0)
    {
      if (!gc->mouseLocked)
      {
        gc->LockMouse();
      }
    }
    else if (!usingJoystick() && gc->mouseLocked)
    {
      gc->UnlockMouse();
    }
  }

  bool usingJoystick() const // C# get-only property -> method
  {
    return false;
  }

  // ============ serialized fields (C# field-block order) ============
  GameControllerScript* gc = 0;
};

} // namespace bb
