#pragma once
#include "../eng.h"
#include "GameControllerScript.hpp"
#include "EntranceScript.hpp"

// 1:1 translation of Assets\Scripts\NearExitTriggerScript.cs.
// NOTE: pulls the Core GameControllerScript translation (real, no longer the placeholder)
// + EntranceScript through its include chain.
// C# note: the original uses bitwise `&` on bools (non-short-circuit); all operands are
// side-effect-free, so `&&` here is behavior-identical and safe.
// Fidelity note: the C# dereferences gc.baldiScrpt unguarded; a defensive null guard was added
// (documented deviation, avoids UB on a stale serialized reference).
namespace bb {

struct NearExitTriggerScript : public Component {
  virtual void OnTriggerEnter(Collider* other) override {
    if (gc->exitsReached < 3 && gc->finaleMode && other->tag() == "Player") {
      gc->ExitReached();
      es->Lower();
      // unity overloaded != null; defensive null guard avoids UB on a stale reference
      if (gc->baldiScrpt && gc->baldiScrpt->isActiveAndEnabled()) gc->baldiScrpt->Hear(transform->position, 8.0f);
    }
  }

  GameControllerScript* gc = 0; // serialized
  EntranceScript* es = 0;       // serialized
};

} // namespace bb
