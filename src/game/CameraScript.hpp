#pragma once
#include "../eng.h"
#include "StubScripts.hpp"   // InputManager stub (Singleton<InputManager>) until the settings batch
#include "PlayerScript.hpp"  // ps.jumpRope / ps.gameOver reads need the complete type

// 1:1 translation of Assets\Scripts\PlayerFunctions\CameraScript.cs (63 lines; camera follows the
// player, hops during jump rope, and frames Baldi for the game-over shot).
//
// Translation notes (fidelity):
// - Quaternion.Euler -> Quat::euler (declared in bbmath; engine impl Phase 2  -  same conversion as
//   setEulerAngles). Quaternion composition is native bbmath Quat::operator*.
// - C# `!ps.gameOver & !ps.jumpRope` -> `&&` (both operands side-effect-free).
// - LateUpdate branch structure preserved; the jumpRope branch IS reachable (branch 1 excludes
//   jumpRope, so non-gameOver + jumpRope lands there).
// - LookAt(...) with one argument uses Unity's default worldUp = (0,1,0)  -  Transform::LookAt matches.
// - `baldi` is a Transform directly: `baldi.transform.position` -> `baldi->position`,
//   `baldi.transform.forward` -> `baldi->forward()`.
// - InputAction.Jump / LookBehind added to types.h (this file is their only user).
// - Fliparoo (GameControllerScript) overwrites `offset` at runtime  -  keep it the serialized Vec3.
// - C# private fields marked `// private (C#)`.

namespace bb {

struct CameraScript : public Component {
  virtual void Start() {
    offset = transform->position - player->transform->position; //Defines the offset
  }

  virtual void Update(float dt) {
    if (ps->jumpRope) //If the player is jump roping
    {
      velocity -= gravity * Time::deltaTime(); //Decrease the velocity using gravity
      jumpHeight += velocity * Time::deltaTime(); //Increase the jump height based on the velocity
      if (jumpHeight <= 0.0f) //When the player is on the floor, prevent the player from falling through.
      {
        jumpHeight = 0.0f;
        // iOS: Rewired GetButton("Jump") / GetButton("LookBack") (ios-ref/classes/CameraScript.ios.c)
        if (ApplicationIsMobilePlatform() ? ReInputPlayer().GetButton("Jump") : Singleton<InputManager>::Instance()->GetActionKey(InputAction::Jump))
        {
          velocity = initVelocity; //Start the jump
        }
      }
      jumpHeightV3 = Vec3(0.0f, jumpHeight, 0.0f); //Turn the float into a vector
    }
    else if (ApplicationIsMobilePlatform() ? ReInputPlayer().GetButton("LookBack") : Singleton<InputManager>::Instance()->GetActionKey(InputAction::LookBehind))
    {
      lookBehind = 180; //Look behind you
    }
    else
    {
      lookBehind = 0; //Don't look behind you
    }
  }

  virtual void LateUpdate() {
    transform->position = player->transform->position + offset; //Teleport to the player, then move based on the offset vector(if all other statements fail)
    if (!ps->gameOver && !ps->jumpRope)
    {
      transform->position = player->transform->position + offset; //Teleport to the player, then move based on the offset vector
      transform->rotation = player->transform->rotation * Quat::euler(Vec3(0.0f, (float)lookBehind, 0.0f)); //Rotate based on player direction + lookbehind
    }
    else if (ps->gameOver)
    {
      transform->position = baldi->position + baldi->forward() * BaldiOffset.z + Vec3(0.0f, BaldiOffset.y, 0.0f); //Puts the camera in front of Baldi
      transform->LookAt(Vec3(baldi->position.x, baldi->position.y + BaldiOffset.y, baldi->position.z)); //Makes the player look at baldi with an offset so the camera doesn't look at the feet
    }
    else if (ps->jumpRope)
    {
      transform->position = player->transform->position + offset + jumpHeightV3; //Apply the jump rope vector onto the normal offset
      transform->rotation = player->transform->rotation; //Rotate based on player direction
    }
  }

  // ============ serialized fields (C# field-block order) ============
  GameObject* player = 0;
  PlayerScript* ps = 0;
  Transform* baldi = 0;
  Vec3 BaldiOffset;
  float initVelocity = 0.0f;
  float velocity = 0.0f;
  float gravity = 0.0f;
  int lookBehind = 0;                  // private (C#)
  Vec3 offset;
  float jumpHeight = 0.0f;
  Vec3 jumpHeightV3;
};

} // namespace bb
