#pragma once
#include "../eng.h"
#include "PlayerScript.hpp" // ps.ResetGuilt below (PlayerScript never references this file: cycle-free)

// 1:1 translation of Assets\Scripts\PlayerFunctions\FacultyTriggerScript.cs (17 lines; entering
// school faculty rooms makes the player guilty of "faculty" for a heartbeat).
// BoxCollider added to eng.h. other.gameObject.CompareTag -> Component::CompareTag on the object.
namespace bb {

struct FacultyTriggerScript : public Component {
  virtual void Start() {
    hitBox = GetComponent<BoxCollider>();
  }

  virtual void OnTriggerStay(Collider* other) {
    if (other->gameObject->CompareTag("Player")) //If it is a player
    {
      ps->ResetGuilt("faculty", 1.0f); //Make the player guilty of entering school faculty for 1 second
    }
  }

  // ============ serialized fields (C# field-block order) ============
  PlayerScript* ps = 0;
  BoxCollider* hitBox = 0;             // private (C#)
};

} // namespace bb
