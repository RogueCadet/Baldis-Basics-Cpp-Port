#pragma once
#include "../eng.h"
#include "CraftersScript.hpp" // crafters.GiveLocation below (one-way include: CraftersScript pulls
                              // GameControllerScript, never this file  -  cycle-free)

// 1:1 translation of Assets\Scripts\NPCFunctions\CraftersTriggerScript.cs (21 lines; room-adjacent
// trigger volumes steer Arts and Crafters toward/flee-from the player's side of the map).
namespace bb {

struct CraftersTriggerScript : public Component {
  virtual void OnTriggerEnter(Collider* other) {
    if (other->tag() == "Player")
    {
      crafters->GiveLocation(goTarget->position, false);
    }
  }

  virtual void OnTriggerExit(Collider* other) {
    if (other->tag() == "Player")
    {
      crafters->GiveLocation(fleeTarget->position, true);
    }
  }

  // ============ serialized fields (C# field-block order) ============
  Transform* goTarget = 0;
  Transform* fleeTarget = 0;
  CraftersScript* crafters = 0;
};

} // namespace bb
