#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\PlayerFunctions\ItemFunctions\BsodaEffectScript.cs (47 lines;
// the push the BSODA stream gives whatever it hits: NPC or player, agent velocity is hijacked).
// Standalone: engine-only dependencies.
// - NavMeshAgent.velocity is a read-write Unity property: getter velocity() (existing callers) +
//   setVelocity() setter (added).
// - Rigidbody type added to eng.h (velocity field; engine Phase 3 integrates).
// - The decompile's OnTriggerExit is Unity's PARAMETERLESS message form; our base hook is the
//   Collider-carrying overload, so the override keeps that signature and ignores the parameter
//   (Unity dispatches both signatures to the same message; behavior identical).
// - C# `&` between side-effect-free compares -> `&&`.
// - C# private members marked `// private (C#)`.
namespace bb {

struct BsodaEffectScript : public Component {
  virtual void Start() {
    agent = GetComponent<NavMeshAgent>(); //Get the object/character's AI Agent
  }

  virtual void Update(float dt) {
    if (inBsoda)
    {
      agent->setVelocity(otherVelocity); //Set the agent's velocity to the velocity of the other object
    }
    if (failSave > 0.0f)
    {
      failSave -= Time::deltaTime();
    }
    else
    {
      inBsoda = false;
    }
  }

  virtual void OnTriggerStay(Collider* other) {
    if (other->tag() == "BSODA") //If its a BSODA
    {
      inBsoda = true;
      otherVelocity = other->GetComponent<Rigidbody>()->velocity; // Set the velocity to the velocity of the BSODA
      failSave = 1.0f;
    }
    else if (other->transform->name() == "Gotta Sweep") //If its Gotta Sweep
    {
      inBsoda = true;
      otherVelocity = transform->forward() * agent->speed * 0.1f + other->GetComponent<NavMeshAgent>()->velocity();
      failSave = 1.0f;
    }
  }

  virtual void OnTriggerExit(Collider* other) {
    inBsoda = false; // When they are out of the BSODA, set inBsoda to false
  }

  // ============ fields (C# field-block order, all private in C#) ============
  NavMeshAgent* agent = 0;             // private (C#)
  Vec3 otherVelocity;                  // private (C#)
  bool inBsoda = false;                // private (C#)
  float failSave = 0.0f;               // private (C#)
};

} // namespace bb
