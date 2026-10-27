#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\NPCFunctions\FirstPrizeSpriteScript.cs (31 lines; 1st Prize's
// billboard: 16 directional sprites picked from the camera-relative orbit angle). Standalone:
// engine-only. sprites[16] exact (C# constructor). Mathf.Sign/Atan2/RoundToInt all exist in eng.h.
namespace bb {

struct FirstPrizeSpriteScript : public Component {
  virtual void Start() {
    sprite = GetComponent<SpriteRenderer>();
  }

  virtual void Update(float dt) {
    angleF = Mathf::Atan2(cam->position.z - body->position.z, cam->position.x - body->position.x) * 57.29578f;
    if (angleF < 0.0f)
    {
      angleF += 360.0f;
    }
    debug = body->eulerAngles().y;
    angleF += body->eulerAngles().y;
    angle = Mathf::RoundToInt(angleF / 22.5f);
    while (angle < 0 || angle >= 16)
    {
      angle += (int)(-16.0f * Mathf::Sign((float)angle));
    }
    sprite->sprite = sprites[angle];
  }

  // ============ serialized fields (C# field-block order) ============
  float debug = 0.0f;
  int angle = 0;
  float angleF = 0.0f;
  SpriteRenderer* sprite = 0;          // private (C#)
  Transform* cam = 0;
  Transform* body = 0;
  Sprite* sprites[16] = {0};           // C# constructor-fixes 16
};

} // namespace bb
