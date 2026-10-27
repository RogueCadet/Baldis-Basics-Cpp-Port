#pragma once
#include "../eng.h"
#include "AILocationSelectorScript.hpp" // wanderer->GetNewTargetHallway() below

// 1:1 translation of Assets\Scripts\NPCFunctions\SweepScript.cs (73 lines; Gotta Sweep wakes up
// every 2-3 minutes and roams the hallways five times, shoving everything out of its path).
// Standalone: no GameControllerScript/PlayerScript references, so straight includes only.
//
// Translation notes (fidelity):
// - FixedUpdate: C# `(double)agent.velocity.magnitude <= 0.1 & coolDown <= 0f & wanders < 5 &
//   active` -> all `&&` (every operand side-effect-free; identical semantics, (double) kept).
// - Random.Range(120f, 180f) is the FLOAT overload -> inclusive [120, 180] (Unity parity).
// - OnTriggerEnter receives Unity's Collider -> other->tag().
// - C# private methods/fields marked `// private (C#)`.

namespace bb {

struct SweepScript : public Component {
  virtual void Start() {
    agent = GetComponent<NavMeshAgent>();
    audioDevice = GetComponent<AudioSource>();
    origin = transform->position;
    waitTime = Random::range(120.0f, 180.0f);
  }

  virtual void Update(float dt) {
    if (coolDown > 0.0f)
    {
      coolDown -= 1.0f * Time::deltaTime();
    }
    if (waitTime > 0.0f)
    {
      waitTime -= Time::deltaTime();
    }
    else if (!active)
    {
      active = true;
      wanders = 0;
      Wander(); // Start wandering
      audioDevice->PlayOneShot(aud_Intro); // "Looks like its sweeping time!"
    }
  }

  virtual void FixedUpdate() {
    if ((double)agent->velocity().magnitude() <= 0.1 && coolDown <= 0.0f && wanders < 5 && active) // If Gotta Sweep has roamed around the school 5 times
    {
      Wander(); // Wander
    }
    else if (wanders >= 5)
    {
      GoHome(); // Go back to the closet
    }
  }

  void Wander() // private (C#)
  {
    wanderer->GetNewTargetHallway();
    agent->SetDestination(wanderTarget->position);
    coolDown = 1.0f;
    wanders++;
  }

  void GoHome() // private (C#)
  {
    agent->SetDestination(origin);
    waitTime = Random::range(120.0f, 180.0f);
    wanders = 0;
    active = false;
  }

  virtual void OnTriggerEnter(Collider* other) {
    if (other->tag() == "NPC" || other->tag() == "Player")
    {
      audioDevice->PlayOneShot(aud_Sweep);
    }
  }

  // ============ serialized fields (C# field-block order) ============
  Transform* wanderTarget = 0;
  AILocationSelectorScript* wanderer = 0;
  float coolDown = 0.0f;
  float waitTime = 0.0f;
  int wanders = 0;
  bool active = false;
  Vec3 origin;                          // private (C#)
  AudioClip* aud_Sweep = 0;
  AudioClip* aud_Intro = 0;
  NavMeshAgent* agent = 0;              // private (C#)
  AudioSource* audioDevice = 0;         // private (C#)
};

} // namespace bb
