#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\PlayerFunctions\ItemFunctions\PickupAnimationScript.cs (13
// lines; bobs a pickup's local Y on a sine each frame). Standalone: engine-only.
// - GetComponent<Transform>() is Unity's no-op self-fetch; kept verbatim (the template instantiates
//   against Transform like any other type).
// - Time.frameCount static field + Mathf::Sin added to eng.h for this file.
namespace bb {

struct PickupAnimationScript : public Component {
  virtual void Start() {
    itemPosition = GetComponent<Transform>();
  }

  virtual void Update(float dt) {
    itemPosition->localPosition = Vec3(0.0f, Mathf::Sin((float)Time::frameCount * 0.017453292f) / 2.0f + 1.0f, 0.0f);
  }

  // ============ fields (C# field-block order) ============
  Transform* itemPosition = 0;         // private (C#)
};

} // namespace bb
