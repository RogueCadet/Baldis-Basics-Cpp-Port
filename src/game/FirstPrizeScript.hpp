#pragma once
#include "../eng.h"
#include "AILocationSelectorScript.hpp" // wanderer->GetNewTargetHallway() below

// 1:1 translation of Assets\Scripts\NPCFunctions\FirstPrizeScript.cs (155 lines; 1st Prize, the
// hug rover with the torque-steering navmesh agent). Standalone: no gc/baldi references, so
// straight includes only.
//
// Translation notes (fidelity):
// - Mathf gained the pure-math Unity pieces used here: DeltaAngle (+Repeat), Abs, Atan2, Sign  - 
//   inlined in eng.h with Unity's exact formulas (Sign(0) == 1; DeltaAngle = shortest arc).
// - NavMeshAgent gained autoBraking (Unity default true), steeringTarget(), destination().
// - Transform gained eulerAngles() (delegates to Quat::eulerAngles) and Rotate() (Unity default
//   Space.Self  -  engine impl Phase 2; every call site here is 1-arg self-space).
// - AudioSource gained the pitch field (motor whine follows agent speed * timescale).
// - The FixedUpdate raycast: origin+direction overload, mask 769, PositiveInfinity range. C#
//   mixed `&` chains -> `&&` (side-effect-free operands; the always-evaluated left raycast owns
//   the hit side-effect  -  same convention as Baldi/Playtime/Bully).
// - Vehicle-flavored serialized fields kept even where unused (str/acceleration/targetRotation/
//   prevSpeed and the commented-out bang block preserved verbatim from the C#).
// - C# private members marked `// private (C#)`.

namespace bb {

struct FirstPrizeScript : public Component {
  virtual void Start() {
    agent = GetComponent<NavMeshAgent>();
    coolDown = 1.0f;
    Wander();
  }

  virtual void Update(float dt) {
    if (coolDown > 0.0f)
    {
      coolDown -= 1.0f * Time::deltaTime();
    }
    if (autoBrakeCool > 0.0f)
    {
      autoBrakeCool -= 1.0f * Time::deltaTime();
    }
    else
    {
      agent->autoBraking = true;
    }
    angleDiff = Mathf::DeltaAngle(transform->eulerAngles().y, Mathf::Atan2(agent->steeringTarget().x - transform->position.x, agent->steeringTarget().z - transform->position.z) * 57.29578f);
    if (crazyTime <= 0.0f)
    {
      if (Mathf::Abs(angleDiff) < 5.0f)
      {
        transform->LookAt(Vec3(agent->steeringTarget().x, transform->position.y, agent->steeringTarget().z));
        agent->speed = currentSpeed;
      }
      else
      {
        transform->Rotate(Vec3(0.0f, turnSpeed * Mathf::Sign(angleDiff) * Time::deltaTime(), 0.0f));
        agent->speed = 0.0f;
      }
    }
    else
    {
      agent->speed = 0.0f;
      transform->Rotate(Vec3(0.0f, 180.0f * Time::deltaTime(), 0.0f));
      crazyTime -= Time::deltaTime();
    }
    motorAudio->pitch = (agent->velocity().magnitude() + 1.0f) * Time::timeScale;
    //if (prevSpeed - agent.velocity.magnitude > 15f)
    //{
    //	audioDevice.PlayOneShot(audBang);
    //}
    //prevSpeed = agent.velocity.magnitude;
  }

  virtual void FixedUpdate() {
    Vec3 direction = player->position - transform->position;
    RaycastHit raycastHit;
    if (Physics::Raycast(transform->position, direction, &raycastHit, std::numeric_limits<float>::infinity(), 769, QueryTriggerInteraction::Ignore) && raycastHit.transform->tag() == "Player")
    {
      if (!playerSeen && !audioDevice->isPlaying())
      {
        int num = Mathf::RoundToInt(Random::range(0.0f, 1.0f));
        audioDevice->PlayOneShot(aud_Found[num]);
      }
      playerSeen = true;
      TargetPlayer();
      currentSpeed = runSpeed;
    }
    else
    {
      currentSpeed = normSpeed;
      if (playerSeen && coolDown <= 0.0f)
      {
        if (!audioDevice->isPlaying())
        {
          int num2 = Mathf::RoundToInt(Random::range(0.0f, 1.0f));
          audioDevice->PlayOneShot(aud_Lost[num2]);
        }
        playerSeen = false;
        Wander();
      }
      else if (agent->velocity().magnitude() <= 1.0f && coolDown <= 0.0f && (transform->position - agent->destination()).magnitude() < 5.0f)
      {
        Wander();
      }
    }
  }

  void Wander() // private (C#)
  {
    wanderer->GetNewTargetHallway();
    agent->SetDestination(wanderTarget->position);
    hugAnnounced = false;
    int num = Mathf::RoundToInt(Random::range(0.0f, 9.0f));
    if (!audioDevice->isPlaying() && num == 0 && coolDown <= 0.0f)
    {
      int num2 = Mathf::RoundToInt(Random::range(0.0f, 1.0f));
      audioDevice->PlayOneShot(aud_Random[num2]);
    }
    coolDown = 1.0f;
  }

  void TargetPlayer() // private (C#)
  {
    agent->SetDestination(player->position);
    coolDown = 0.5f;
  }

  virtual void OnTriggerEnter(Collider* other) {
    if (other->tag() == "Player")
    {
      if (!audioDevice->isPlaying() && !hugAnnounced)
      {
        int num = Mathf::RoundToInt(Random::range(0.0f, 1.0f));
        audioDevice->PlayOneShot(aud_Hug[num]);
        hugAnnounced = true;
      }
      agent->autoBraking = false;
    }
  }

  virtual void OnTriggerExit(Collider* other) {
    if (other->tag() == "Player")
    {
      autoBrakeCool = 1.0f;
    }
  }

  void GoCrazy()
  {
    crazyTime = 15.0f;
  }

  // ============ serialized fields (C# field-block order) ============
  float debug = 0.0f;
  float turnSpeed = 0.0f;
  float str = 0.0f;
  float angleDiff = 0.0f;
  float normSpeed = 0.0f;
  float runSpeed = 0.0f;
  float currentSpeed = 0.0f;
  float acceleration = 0.0f;
  float speed = 0.0f;
  float autoBrakeCool = 0.0f;
  float crazyTime = 0.0f;
  Quat targetRotation;
  float coolDown = 0.0f;
  float prevSpeed = 0.0f;              // private (C#)
  bool playerSeen = false;
  bool hugAnnounced = false;
  AILocationSelectorScript* wanderer = 0;
  Transform* player = 0;
  Transform* wanderTarget = 0;
  AudioClip* aud_Found[2] = {0, 0};    // C# constructor-fixes 2
  AudioClip* aud_Lost[2] = {0, 0};
  AudioClip* aud_Hug[2] = {0, 0};
  AudioClip* aud_Random[2] = {0, 0};
  AudioClip* audBang = 0;
  AudioSource* audioDevice = 0;
  AudioSource* motorAudio = 0;
  NavMeshAgent* agent = 0;             // private (C#)
};

} // namespace bb
