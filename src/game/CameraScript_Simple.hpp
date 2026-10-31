#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\PlayerFunctions\CameraScript_Simple.cs (18 lines; the 31718
// easter-egg TestRoom camera: rigid follow of the PlayerMovement-controlled player, no jump rope
// and no game-over framing -- that logic lives in CameraScript for the real school).
//
// Translation notes: Quaternion.Euler(0, lookBehind, 0) -> Quat::euler(Vec3(0, lookBehind, 0))
// (same conversion CameraScript uses); C# private fields marked `// private (C#)`.
namespace bb {

struct CameraScript_Simple : public Component {
  virtual void Start()
  {
    offset = transform->position - player->transform->position; //Defines the offset
  }

  virtual void LateUpdate()
  {
    transform->position = player->transform->position + offset; //Teleport to the player, then move based on the offset vector
    transform->rotation = player->transform->rotation * Quat::euler(Vec3(0.0f, (float)lookBehind, 0.0f)); //Rotate based on player direction
  }

  // ============ serialized fields (C# field-block order) ============
  GameObject* player = 0;

  // private fields (C#)
  int lookBehind = 0;
  Vec3 offset;
};

} // namespace bb
