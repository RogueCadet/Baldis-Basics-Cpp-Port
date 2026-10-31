#pragma once
#include "../eng.h"
#include "StubScripts.hpp"
#include "AILocationSelectorScript.hpp" // wanderer->GetNewTargetHallway() below
#include "PlayerScript.hpp"             // ps.jumpRope read needs the complete type

// 1:1 translation of Assets\Scripts\NPCFunctions\PlaytimeScript.cs (114 lines; the jump-rope
// stalker). She hunts the player through the FixedUpdate raycast every 80 units or less for 15
// seconds, then demands a rope round.
//
// Translation notes (fidelity):
// - Engine additions this file forced: Animator::GetBool (flag table, hand-coded anim Phase 2)
//   and NavMeshAgent::Warp (back-off teleport after touching the player).
// - FixedUpdate raycast mirrors BaldiScript: origin+direction overload, mask 769, infinity range,
//   `&&` conversion of the C# mixed `&` chain (raycast first owns the hit side-effect; the tag
//   compare is unreachable on a miss  -  as established at BaldiScript).
// - agent.speed is runtime state (15 wander / 20 chase / 0 pinned during rope).
// - aud_Numbers[10] / aud_Random[2] sizes are constructor-mandated in the C# (new AudioClip[n]).
// - StubScripts retired its PlaytimeScript stub BEFORE GC wiring; PlayerScript keeps a fwd decl
//   for its pointer field, and its out-of-line OnTriggerEnter body (playtime->playCool read)
//   compiles against the real file via the GameControllerScript include chain.
// - C# private members marked `// private (C#)`; `aud_Instrcutions` misspelling kept (decompile).

namespace bb {

struct PlaytimeScript : public Component {
  virtual void Start() {
    agent = GetComponent<NavMeshAgent>(); //Get AI Agent
    audioDevice = GetComponent<AudioSource>();
    Wander(); //Start wandering
  }

  virtual void Update(float dt) {
    // -debug cheat ("make NPCs ignore you"): her timers freeze and she never switches to hunt mode.
    if (cheatNpcIgnore()) return;
    if (coolDown > 0.0f)
    {
      coolDown -= 1.0f * Time::deltaTime();
    }
    if (playCool >= 0.0f)
    {
      playCool -= Time::deltaTime();
    }
    else if (animator->GetBool("disappointed"))
    {
      playCool = 0.0f;
      animator->SetBool("disappointed", false);
    }
  }

  virtual void FixedUpdate() {
    // -debug cheat: the hunt raycast and the chase agent stop with the cheat on.
    if (cheatNpcIgnore()) return;
    if (!ps->jumpRope)
    {
      Vec3 direction = player->position - transform->position;
      RaycastHit raycastHit;
      if (Physics::Raycast(transform->position, direction, &raycastHit, std::numeric_limits<float>::infinity(), 769, QueryTriggerInteraction::Ignore) && raycastHit.transform->tag() == "Player" && (transform->position - player->position).magnitude() <= 80.0f && playCool <= 0.0f)
      {
        playerSeen = true; //If playtime sees the player, she chases after them
        TargetPlayer();
      }
      else if (playerSeen && coolDown <= 0.0f)
      {
        playerSeen = false; //If the player seen cooldown expires, she will just start wandering again
        Wander();
      }
      else if (agent->velocity().magnitude() <= 1.0f && coolDown <= 0.0f)
      {
        Wander();
      }
      jumpRopeStarted = false;
    }
    else
    {
      if (!jumpRopeStarted)
      {
        agent->Warp(transform->position - transform->forward() * 10.0f); //Teleport back after touching the player
      }
      jumpRopeStarted = true;
      agent->speed = 0.0f;
      playCool = 15.0f;
    }
  }

  void Wander() // private (C#)
  {
    wanderer->GetNewTargetHallway();
    agent->SetDestination(wanderTarget->position);
    agent->speed = 15.0f;
    playerSpotted = false;
    audVal = Mathf::RoundToInt(Random::range(0.0f, 1.0f));
    if (!audioDevice->isPlaying())
    {
      audioDevice->PlayOneShot(aud_Random[audVal]);
    }
    coolDown = 1.0f;
  }

  void TargetPlayer() // private (C#)
  {
    animator->SetBool("disappointed", false); //No longer be sad
    agent->SetDestination(player->position); // Go after the player
    agent->speed = 20.0f; // Speed up
    coolDown = 0.2f;
    if (!playerSpotted)
    {
      playerSpotted = true;
      audioDevice->PlayOneShot(aud_LetsPlay);
    }
  }

  void Disappoint()
  {
    animator->SetBool("disappointed", true); //Get sad
    audioDevice->Stop();
    audioDevice->PlayOneShot(aud_Sad);
  }

  // ============ serialized fields (C# field-block order) ============
  bool db = false;
  bool playerSeen = false;
  bool disappointed = false;
  int audVal = 0;
  Animator* animator = 0;
  Transform* player = 0;
  PlayerScript* ps = 0;
  Transform* wanderTarget = 0;
  AILocationSelectorScript* wanderer = 0;
  float coolDown = 0.0f;
  float playCool = 0.0f;
  bool playerSpotted = false;
  bool jumpRopeStarted = false;
  NavMeshAgent* agent = 0;             // private (C#)
  AudioClip* aud_Numbers[10] = {0};    // C# constructor-fixes 10
  AudioClip* aud_Random[2] = {0, 0};   // C# constructor-fixes 2
  AudioClip* aud_Instrcutions = 0;     // [sic] decompile spelling kept
  AudioClip* aud_Oops = 0;
  AudioClip* aud_LetsPlay = 0;
  AudioClip* aud_Congrats = 0;
  AudioClip* aud_ReadyGo = 0;
  AudioClip* aud_Sad = 0;
  AudioSource* audioDevice = 0;        // public (JumpRopeScript drives it directly)
};

} // namespace bb
