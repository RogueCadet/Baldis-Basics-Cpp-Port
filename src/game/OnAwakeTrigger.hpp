#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\OnAwakeTrigger.cs (10 lines; a scene-authoring trigger that
// fires a UnityEvent the moment its object enables).
// - UnityEvent -> engine type added to eng.h: Invoke() runs the scene-bound persistent listener
//   list (filled by the Phase 1 scene-data binding, exactly how Unity deserializes it).
// - OnEnable is a new Component hook (Unity calls it whenever the object is enabled; Phase 2
//   scheduler order: OnEnable then Start on first enable).
namespace bb {

struct OnAwakeTrigger : public Component {
  virtual void OnEnable() { // private (C#)
    OnEnableEvent.Invoke();
  }

  // ============ serialized fields (C# field-block order) ============
  UnityEvent OnEnableEvent;
};

} // namespace bb
