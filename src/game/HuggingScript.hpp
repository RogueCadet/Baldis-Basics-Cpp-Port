#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\NPCFunctions\HuggingScript.cs (44 lines; hugs-the-player
// counterpart of BsodaEffect: 1st Prize's hug pushes via Rigidbody velocity transfer).
// Standalone: engine-only. Rigidbody added to eng.h. The decompile's OnTriggerExit is Unity's
// PARAMETERLESS message form -> override keeps the Collider*-carrying base signature and ignores
// the param (same convention as BsodaEffectScript).
namespace bb {

struct HuggingScript : public Component {
  virtual void Start() {
    rb = GetComponent<Rigidbody>();
  }

  virtual void Update(float dt) {
    if (failSave > 0.0f)
    {
      failSave -= Time::deltaTime();
    }
    else
    {
      inBsoda = false;
    }
  }

  virtual void FixedUpdate() {
    if (inBsoda)
    {
      rb->velocity = otherVelocity;
    }
  }

  virtual void OnTriggerStay(Collider* other) {
    if (other->transform->name() == "1st Prize")
    {
      inBsoda = true;
      otherVelocity = rb->velocity * 0.1f + other->GetComponent<NavMeshAgent>()->velocity();
      failSave = 1.0f;
    }
  }

  virtual void OnTriggerExit(Collider* other) {
    inBsoda = false;
  }

  // ============ fields (C# field-block order) ============
  Rigidbody* rb = 0;                   // private (C#)
  Vec3 otherVelocity;                  // private (C#)
  bool inBsoda = false;
  float failSave = 0.0f;               // private (C#)
};

} // namespace bb
