#pragma once
#include "../eng.h"
#include "AILocationSelectorScript.hpp" // wanderer->GetNewTarget() below

// 1:1 translation of Assets\Scripts\NPCFunctions\AgentTest.cs (68 lines; the TestRoom's demo
// chaser: wanders between the demo scene's AI wander points and homes in whenever the fixed-tick
// line-of-sight raycast hits the player -- proof-of-concept for the NPC agent behaviour).
//
// Translation notes: FixedUpdate is the engine's no-arg fixed hook (same shape as BaldiScript);
// the unnormalized raycast direction is passed through exactly like BaldiScript's (the engine
// Raycast normalizes; Unity's does too for the max-distance contract, and maxDistance is
// infinity here); C# `&` between side-effect-free bools -> `&&`; layerMask 3 verbatim.
namespace bb {

struct AgentTest : public Component {
  virtual void Start()
  {
    agent = GetComponent<NavMeshAgent>(); // Define the AI Agent
    Wander(); //Start wandering
  }
  virtual void Update(float dt)
  {
    if (coolDown > 0.0f)
    {
      coolDown -= 1.0f * Time::deltaTime();
    }
  }
  virtual void FixedUpdate()
  {
    Vec3 direction = player->position - transform->position;
    RaycastHit raycastHit;
    if (Physics::Raycast(transform->position, direction, &raycastHit, std::numeric_limits<float>::infinity(), 3, QueryTriggerInteraction::Ignore) && raycastHit.transform->tag() == "Player") //Check if its the player
    {
      db = true;
      TargetPlayer(); //Head towards the player
    }
    else
    {
      db = false;
      if (agent->velocity().magnitude() <= 1.0f && coolDown <= 0.0f)
      {
        Wander(); //Just wander
      }
    }
  }
  void Wander() // private (C#)
  {
    wanderer->GetNewTarget(); //Get a new target on the map
    agent->SetDestination(wanderTarget->position); //Set its destination to position of the wanderTarget
    coolDown = 1.0f;
  }
  void TargetPlayer() // private (C#)
  {
    agent->SetDestination(player->position); //Set it's destination to the player
    coolDown = 1.0f;
  }

  // ============ serialized fields (C# field-block order) ============
  bool db = false;
  Transform* player = 0;
  Transform* wanderTarget = 0;
  AILocationSelectorScript* wanderer = 0;
  float coolDown = 0.0f;

  NavMeshAgent* agent = 0; // private (C#)
};

} // namespace bb
