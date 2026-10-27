#pragma once
#include "../eng.h"
#include "UIController.hpp" // uc member writes + SwitchMenu below (one-way include; UIController
                            // never references MenuController)

// 1:1 translation of Assets\Scripts\Core\MenuController.cs (17 lines; each menu rebinds the
// shared UIController's selection targets and re-routes focus on enable).
// Note: `public void OnEnable` is the Unity message (fires per enable), not a plain method.
namespace bb {

struct MenuController : public Component {
  virtual void OnEnable() {
    uc->firstButton = firstButton;
    uc->dummyButtonPC = dummyButtonPC;
    uc->dummyButtonElse = dummyButtonElse;
    uc->SwitchMenu();
  }

  // ============ serialized fields (C# field-block order) ============
  UIController* uc = 0;
  Selectable* firstButton = 0;
  Selectable* dummyButtonPC = 0;
  Selectable* dummyButtonElse = 0;
  GameObject* back = 0;
};

} // namespace bb
