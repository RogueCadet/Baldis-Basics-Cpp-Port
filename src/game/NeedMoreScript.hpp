#pragma once
#include "../eng.h"
#include "GameControllerScript.hpp" // gc.notebooks read (one-way; GC never references this type)

// 1:1 translation of Assets\Scripts\NeedMoreScript.cs (14 lines; "you need more notebooks" idle
// voice line when the player pokes an exit early). C# `&` between side-effect-free compares -> &&.
namespace bb {

struct NeedMoreScript : public Component {
  virtual void OnTriggerEnter(Collider* other) {
    if (gc->notebooks < 2 && other->tag() == "Player")
    {
      audioDevice->PlayOneShot(baldiDoor, 1.0f);
    }
  }

  // ============ serialized fields (C# field-block order) ============
  GameControllerScript* gc = 0;
  AudioSource* audioDevice = 0;
  AudioClip* baldiDoor = 0;
};

} // namespace bb
