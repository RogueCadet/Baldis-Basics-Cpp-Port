#pragma once
#include "../eng.h"
#include "PlayerScript.hpp"             // playerScript.guilt / guiltType / principalBugFixer
#include "BullyScript.hpp"              // bullyScript.guilt
#include "BaldiScript.hpp"              // baldiScript.isActiveAndEnabled / Hear
#include "AILocationSelectorScript.hpp" // wanderer->GetNewTarget()
#include "DoorScript.hpp"               // officeDoor.LockDoor(...)
#include "AudioQueueScript.hpp"         // audioQueue->QueueAudio / ClearAudioQueue

// 1:1 translation of Assets\Scripts\NPCFunctions\PrincipalScript.cs (200 lines; Principal of the
// Thing: catches rule breakers, detains in his office with escalating lock times, escorts the
// Bully out). No gc reference at all  -  every dependency is a direct one-way include, so all
// bodies stay inline (no include-cycle work).
//
// Translation notes (fidelity):
// - `lockTime` (C# private int[] {15,30,45,60,99}) is a constructor-initialized member  -  exact.
// - Physics.Raycast got the 5-arg overload (no explicit QueryTriggerInteraction = UseGlobal)  - 
//   the bully-check raycast uses it; the player-check raycast uses the 6-arg form.
// - NavMeshAgent.isStopped became a field (Unity property; engine Phase 4 honors it).
// - other.name (Collider's Component.name) -> Component::name() added to eng.h.
// - `&` chains of side-effect-free bools -> `&&` (mixed chains preserved as in C#).
// - `random.Range(0f, 2f)` is the float overload -> inclusive [0..2] for audScolds[3].
// - C# private members marked `// private (C#)`.

namespace bb {

struct PrincipalScript : public Component {
  virtual void Start() {
    agent = GetComponent<NavMeshAgent>(); //Get the agent
    audioQueue = GetComponent<AudioQueueScript>();
    audioDevice = GetComponent<AudioSource>();
  }

  virtual void Update(float dt) {
    // -debug cheat ("make NPCs ignore you"): no scolding, no detentions, no chasing.
    if (cheatNpcIgnore()) return;
    if (seesRuleBreak)
    {
      timeSeenRuleBreak += 1.0f * Time::deltaTime();
      if ((double)timeSeenRuleBreak >= 0.5 && !angry) // If the principal sees the player break a rule for more then 1/2 of a second
      {
        angry = true;
        seesRuleBreak = false;
        timeSeenRuleBreak = 0.0f;
        TargetPlayer(); //Target the player
        CorrectPlayer();
      }
    }
    else
    {
      timeSeenRuleBreak = 0.0f;
    }
    if (coolDown > 0.0f)
    {
      coolDown -= 1.0f * Time::deltaTime();
    }
  }

  virtual void FixedUpdate() {
    // -debug cheat: the guilt raycast and the wander agent both stop with the world frozen.
    if (cheatNpcIgnore()) return;
    if (!angry) // If the principal isn't angry
    {
      aim = player->position - transform->position; // If he sees the player and the player has guilt
      if (Physics::Raycast(transform->position, aim, &hit, std::numeric_limits<float>::infinity(), 769, QueryTriggerInteraction::Ignore) && hit.transform->tag() == "Player" && playerScript->guilt > 0.0f && !inOffice && !angry)
      {
        seesRuleBreak = true;
      }
      else
      {
        seesRuleBreak = false;
        if (agent->velocity().magnitude() <= 1.0f && coolDown <= 0.0f)
        {
          Wander(); // Start wandering again
        }
      }
      aim = bully->position - transform->position;
      if (Physics::Raycast(transform->position, aim, &hit, std::numeric_limits<float>::infinity(), 769) && hit.transform->name() == "Its a Bully" && bullyScript->guilt > 0.0f && !inOffice && !angry)
      {
        TargetBully();
      }
    }
    else
    {
      TargetPlayer(); // If the principal is angry, target the player
    }
  }

  void Wander() // private (C#)
  {
    playerScript->principalBugFixer = 1;
    wanderer->GetNewTarget();
    agent->SetDestination(wanderTarget->position);
    if (agent->isStopped)
    {
      agent->isStopped = false;
    }
    coolDown = 1.0f;
    if (Random::range(0.0f, 10.0f) <= 1.0f)
    {
      quietAudioDevice->PlayOneShot(aud_Whistle);
    }
  }

  void TargetPlayer() // private (C#)
  {
    agent->SetDestination(player->position);
    coolDown = 1.0f;
  }

  void TargetBully() // private (C#)
  {
    if (!bullySeen)
    {
      agent->SetDestination(bully->position);
      audioQueue->QueueAudio(audNoBullying);
      bullySeen = true;
    }
  }

  void CorrectPlayer() // private (C#)
  {
    audioQueue->ClearAudioQueue();
    if (playerScript->guiltType == "faculty")
    {
      audioQueue->QueueAudio(audNoFaculty);
    }
    else if (playerScript->guiltType == "running")
    {
      audioQueue->QueueAudio(audNoRunning);
    }
    //else if (playerScript.guiltType == "food")
    //{
    //audioQueue.QueueAudio(audNoEating);
    //}
    else if (playerScript->guiltType == "drink")
    {
      audioQueue->QueueAudio(audNoDrinking);
    }
    else if (playerScript->guiltType == "escape")
    {
      audioQueue->QueueAudio(audNoEscaping);
    }
  }

  virtual void OnTriggerStay(Collider* other) { // private (C#)
    if (other->gameObject->name == "Office Trigger")
    {
      inOffice = true;
    }
    if (other->tag() == "Player" && angry)
    {
      inOffice = true;
      playerScript->principalBugFixer = 0;
      agent->Warp(principalDetentionPoint->position); //Teleport the principal to Principal_DetentionPoint
      agent->isStopped = true; //Stop the principal from moving
      other->transform->position = Vec3(playerDetentionPoint->position.x, 4.0f, playerDetentionPoint->position.z); // Teleport the player to Player_DetentionPoint
      audioQueue->QueueAudio(aud_Delay);
      audioQueue->QueueAudio(audTimes[detentions]); //Play the detention time sound
      audioQueue->QueueAudio(audDetention);
      int num = Mathf::RoundToInt(Random::range(0.0f, 2.0f));
      audioQueue->QueueAudio(audScolds[num]); // Say one of the other lines
      officeDoor->LockDoor((float)lockTime[detentions]); // Lock the door
      if (baldiScript->isActiveAndEnabled()) baldiScript->Hear(transform->position, 8.0f);
      coolDown = 5.0f;
      angry = false;
      detentions++;
      if (detentions > 4) // Prevent detention number from going above 4
      {
        detentions = 4;
      }
    }
  }

  virtual void OnTriggerExit(Collider* other) { // private (C#)
    if (other->gameObject->name == "Office Trigger")
    {
      inOffice = false;
    }
    if (other->gameObject->name == "Its a Bully")
    {
      bullySeen = false;
    }
  }

  // ============ serialized fields (C# field-block order) ============
  bool seesRuleBreak = false;
  Transform* player = 0;
  Transform* bully = 0;
  bool bullySeen = false;
  PlayerScript* playerScript = 0;
  BullyScript* bullyScript = 0;
  BaldiScript* baldiScript = 0;
  Transform* wanderTarget = 0;
  AILocationSelectorScript* wanderer = 0;
  DoorScript* officeDoor = 0;
  Transform* principalDetentionPoint = 0;
  Transform* playerDetentionPoint = 0;
  float coolDown = 0.0f;
  float timeSeenRuleBreak = 0.0f;
  bool angry = false;
  bool inOffice = false;
  int detentions = 0;                  // private (C#)
  int lockTime[5] = {15, 30, 45, 60, 99}; // private (C#)  -  exact C# initializer
  AudioClip* audTimes[5] = {0};        // C# constructor-fixes 5
  AudioClip* audScolds[3] = {0};       // C# constructor-fixes 3
  AudioClip* audDetention = 0;
  AudioClip* audNoDrinking = 0;
  AudioClip* audNoEating = 0;
  AudioClip* audNoBullying = 0;
  AudioClip* audNoFaculty = 0;
  AudioClip* audNoLockers = 0;
  AudioClip* audNoRunning = 0;
  AudioClip* audNoStabbing = 0;
  AudioClip* audNoEscaping = 0;
  AudioClip* aud_Whistle = 0;
  AudioClip* aud_Delay = 0;
  NavMeshAgent* agent = 0;             // private (C#)
  AudioQueueScript* audioQueue = 0;    // private (C#)
  AudioSource* audioDevice = 0;        // private (C#)
  AudioSource* quietAudioDevice = 0;
  RaycastHit hit;                      // private (C#)
  Vec3 aim;                            // private (C#)
  CharacterController* cc = 0;
};

} // namespace bb
