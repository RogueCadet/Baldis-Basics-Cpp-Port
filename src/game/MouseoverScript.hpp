#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\MouseoverScript.cs (24 lines; uGUI hover/selection sound
// bridge: select-or-enter -> mouseOver event, deselect-or-exit -> mouseLeave event).
// - The four Unity event-system interfaces (ISelectHandler/IPointerEnterHandler/IDeselectHandler/
//   IPointerExitHandler) are just virtual handlers the Phase 2 UI layer dispatches to; eventData
//   pointers carried as void* (nothing in this script touches their members).
// - UnityEvents are engine types (Phase 1 scene binding fills their listener lists).
namespace bb {

struct MouseoverScript : public Component {
  void OnSelect(void* eventData)
  {
    mouseOver.Invoke();
  }

  void OnPointerEnter(void* eventData)
  {
    mouseOver.Invoke();
  }

  void OnDeselect(void* eventData)
  {
    mouseLeave.Invoke();
  }

  void OnPointerExit(void* eventData)
  {
    mouseLeave.Invoke();
  }

  // ============ serialized fields (C# field-block order) ============
  UnityEvent mouseOver;
  UnityEvent mouseLeave;
};

} // namespace bb
