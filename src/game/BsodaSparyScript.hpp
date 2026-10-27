#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\PlayerFunctions\ItemFunctions\BsodaSparyScript.cs (23 lines;
// [sic] the BSODA spray projectile: flies straight at `speed`, dies after 30 s).
// Standalone: engine-only dependencies. Rigidbody added to eng.h (velocity field, Phase 3 motion).
// C# private members marked `// private (C#)`.
namespace bb {

struct BsodaSparyScript : public Component {
  virtual void Start() {
    rb = GetComponent<Rigidbody>(); //Get the RigidBody
    rb->velocity = transform->forward() * speed; //Move forward
    lifeSpan = 30.0f; //Set the lifespan
  }

  virtual void Update(float dt) {
    rb->velocity = transform->forward() * speed; //Move forward
    lifeSpan -= Time::deltaTime(); // Decrease the lifespan variable
    if (lifeSpan < 0.0f) //When the lifespan timer ends, destroy the BSODA
    {
      Destroy(gameObject, 0.0f);
    }
  }

  // ============ fields (C# field-block order) ============
  float speed = 0.0f;
  float lifeSpan = 0.0f;               // private (C#)
  Rigidbody* rb = 0;                   // private (C#)
};

} // namespace bb
