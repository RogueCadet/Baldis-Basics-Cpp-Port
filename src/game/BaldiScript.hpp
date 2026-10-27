#pragma once
#include "../eng.h"
#include "StubScripts.hpp"
#include "AILocationSelectorScript.hpp" // real translation (wanderer field + GetNewTarget)

// 1:1 translation of Assets\Scripts\NPCFunctions\BaldiScript.cs (171 lines; the ruler-slap chase
// AI). Clean fit: no GameControllerScript dependency (unlike most NPC scripts), so the stub
// retired with no include-cycle work. Every method/body inline.
//
// Translation notes (fidelity):
// - FixedUpdate is a new MonoBehaviour hook on Component (engine runs fixed-timestep updates;
//   Phase 2 scheduler gives it Unity's tick cadence).
// - Physics.Raycast got Unity's origin+direction overload: (origin, dir, out hit, maxDistance,
//   layerMask, QueryTriggerInteraction)  -  layerMask 769 kept as the raw Unity value (layers 0, 8, 9:
//   Default + the NPC/occluder layers). float.PositiveInfinity -> std::numeric_limits infinity.
// - C# `Physics.Raycast(...) & raycastHit.transform.tag == "Player"` -> `&&`. The raycast (left
//   operand) always evaluates first and owns the hit side-effect; on a miss Unity leaves
//   hit.transform null and the tag compare would NRE  -  unreachable in practice because the mask
//   always contains the player/occluder layers, so && is behavior-identical here and safer.
// - The rumble block computes MobileController-style vibration levels but this decompile build
//   never sends them (dead locals kept verbatim: num feeds the range check; motorLevel discarded).
// - The anger formula is pure arithmetic and kept exactly as decompiled.
// - C# private methods/fields marked `// private (C#)`.

// engine impl header size limit marker  -  include <limits> for the infinity constant
#include <limits>

namespace bb {

struct BaldiScript : public Component {
  // ============ methods (C# listing order) ============

  virtual void Start() {
    baldiAudio = GetComponent<AudioSource>(); //Get The Baldi Audio Source(Used mostly for the slap sound)
    agent = GetComponent<NavMeshAgent>(); //Get the Nav Mesh Agent
    timeToMove = baseTime; //Sets timeToMove to baseTime
    Wander(); //Start wandering
    if (PlayerPrefs::GetInt("Rumble") == 1)
    {
      rumble = true;
    }
  }

  virtual void Update(float dt) {
    if (timeToMove > 0.0f) //If timeToMove is greater then 0, decrease it
    {
      timeToMove -= 1.0f * Time::deltaTime();
    }
    else
    {
      Move(); //Start moving
    }
    if (coolDown > 0.0f) //If coolDown is greater then 0, decrease it
    {
      coolDown -= 1.0f * Time::deltaTime();
    }
    if (baldiTempAnger > 0.0f) //Slowly decrease Baldi's temporary anger over time.
    {
      baldiTempAnger -= 0.02f * Time::deltaTime();
    }
    else
    {
      baldiTempAnger = 0.0f; //Cap its lowest value at 0
    }
    if (antiHearingTime > 0.0f) //Decrease antiHearingTime, then when it runs out stop the effects of the antiHearing tape
    {
      antiHearingTime -= Time::deltaTime();
    }
    else
    {
      antiHearing = false;
    }
    if (endless) //Only activate if the player is playing on endless mode
    {
      if (timeToAnger > 0.0f) //Decrease the timeToAnger
      {
        timeToAnger -= 1.0f * Time::deltaTime();
      }
      else
      {
        timeToAnger = angerFrequency; //Set timeToAnger to angerFrequency
        GetAngry(angerRate); //Get angry based on angerRate
        angerRate += angerRateRate; //Increase angerRate for next time
      }
    }
  }

  virtual void FixedUpdate() {
    if (moveFrames > 0.0f) //Move for a certain amount of frames, and then stop moving.(Ruler slapping)
    {
      moveFrames -= 1.0f;
      agent->speed = speed;
    }
    else
    {
      agent->speed = 0.0f;
    }
    Vec3 direction = player->position - transform->position;
    RaycastHit raycastHit;
    if (Physics::Raycast(transform->position + Vec3(0.0f, 1.0f, 0.0f) * 2.0f, direction, &raycastHit, std::numeric_limits<float>::infinity(), 769, QueryTriggerInteraction::Ignore) && raycastHit.transform->tag() == "Player") //Create a raycast, if the raycast hits the player, Baldi can see the player
    {
      db = true;
      TargetPlayer(); //Start attacking the player
    }
    else
    {
      db = false;
    }
  }

  void Wander() // private (C#)
  {
    wanderer->GetNewTarget(); //Get a new location
    agent->SetDestination(wanderTarget->position); //Head towards the position of the wanderTarget object
    coolDown = 1.0f; //Set the cooldown
    currentPriority = 0.0f;
  }

  void TargetPlayer()
  {
    agent->SetDestination(player->position); //Target the player
    coolDown = 1.0f;
    currentPriority = 0.0f;
  }

  void Move() // private (C#)
  {
    if (transform->position == previous && coolDown < 0.0f) // If Baldi reached his destination, start wandering
    {
      Wander();
    }
    moveFrames = 10.0f;
    timeToMove = baldiWait - baldiTempAnger;
    previous = transform->position; // Set previous to Baldi's current location
    baldiAudio->PlayOneShot(slap); //Play the slap sound
    baldiAnimator->SetTrigger("slap"); // Play the slap animation
    if (rumble)
    {
      float num = dist(transform->position, player->position);
      if (num < vibrationDistance)
      {
        float motorLevel = 1.0f - num / vibrationDistance; // decompile: vibration send stripped on this platform (dead local kept)
      }
    }
  }

  void GetAngry(float value)
  {
    baldiAnger += value; // Increase Baldi's anger by the value provided
    if (baldiAnger < 0.5f) //Cap Baldi anger at a minimum of 0.5
    {
      baldiAnger = 0.5f;
    }
    baldiWait = -3.0f * baldiAnger / (baldiAnger + 2.0f / baldiSpeedScale) + 3.0f; //Some formula I don't understand.
  }

  void GetTempAngry(float value)
  {
    baldiTempAnger += value; //Increase Baldi's Temporary Anger
  }

  void Hear(const Vec3& soundLocation, float priority)
  {
    if (!antiHearing && priority >= currentPriority) //If anti-hearing is not active and the priority is greater then the priority of the current sound
    {
      agent->SetDestination(soundLocation); //Go to that sound
      currentPriority = priority; //Set the current priority to the priority
    }
  }

  void ActivateAntiHearing(float t)
  {
    Wander(); //Start wandering
    antiHearing = true; //Set the antihearing variable to true for other scripts
    antiHearingTime = t; //Set the time the tape's effect on baldi will last
  }

  // ============ serialized fields (C# field-block order) ============
  bool db = false;
  float baseTime = 0.0f;
  float speed = 0.0f;
  float timeToMove = 0.0f;
  float baldiAnger = 0.0f;
  float baldiTempAnger = 0.0f;
  float baldiWait = 0.0f;
  float baldiSpeedScale = 0.0f;
  float moveFrames = 0.0f;             // private (C#)
  float currentPriority = 0.0f;        // private (C#)
  bool antiHearing = false;
  float antiHearingTime = 0.0f;
  float vibrationDistance = 0.0f;
  float angerRate = 0.0f;
  float angerRateRate = 0.0f;
  float angerFrequency = 0.0f;
  float timeToAnger = 0.0f;
  bool endless = false;
  Transform* player = 0;
  Transform* wanderTarget = 0;
  AILocationSelectorScript* wanderer = 0;
  AudioSource* baldiAudio = 0;         // private (C#)
  AudioClip* slap = 0;
  Animator* baldiAnimator = 0;
  float coolDown = 0.0f;
  Vec3 previous;                       // private (C#)
  bool rumble = false;                 // private (C#)
  NavMeshAgent* agent = 0;             // private (C#)
};

} // namespace bb
