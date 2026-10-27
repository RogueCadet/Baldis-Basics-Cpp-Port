#pragma once
#include "../eng.h"
#include "AILocationSelectorScript.hpp" // wanderer.QuarterExclusive() below

// 1:1 translation of Assets\Scripts\PlayerFunctions\ItemFunctions\QuarterSpawnScript.cs (11
// lines; the quarter pickup: exclusive spawn slot 1..15, then +4 on Y). Standalone.
// Vector3.up * 4f -> Vec3(0,1,0) * 4 (bbmath has no Vector3.up alias; math identical).
namespace bb {

struct QuarterSpawnScript : public Component {
  virtual void Start() {
    wanderer->QuarterExclusive();
    transform->position = location->position + Vec3(0.0f, 1.0f, 0.0f) * 4.0f;
  }

  // ============ serialized fields (C# field-block order) ============
  AILocationSelectorScript* wanderer = 0;
  Transform* location = 0;
};

} // namespace bb
