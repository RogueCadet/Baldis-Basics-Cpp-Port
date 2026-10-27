#pragma once
#include "../eng.h"
#include "StubScripts.hpp"
#include "AILocationSelectorScript.hpp" // wanderer->GetNewTargetHallway() below
#include "GameControllerScript.hpp"     // gc.item[] reads + LoseItem() below (one-way: GC never
                                        // references BullyScript, so a direct include is cycle-free)

// 1:1 translation of Assets\Scripts\NPCFunctions\BullyScript.cs (109 lines; Here Comes The Bully:
// steals an item for safe passage, gets reset by the Principal when guilty).
//
// Translation notes (fidelity):
// - Random.Range(0f, 2f) is the FLOAT overload -> inclusive [0..2] then re-rolled while the slot
//   is empty (C# comment calls it "a random item slot"; semantics preserved verbatim, loop
//   included  -  safe because the enclosing branch guarantees a non-empty inventory).
// - `&` chains of side-effect-free bools (activeTime/ray tag/distance/active...) -> `&&`.
// - other.transform.tag / .name -> other->transform->tag() / ->name() (Unity Component members).
// - FixedUpdate raycast uses Unity's origin+direction overload, mask 769, PositiveInfinity range
//   (same convention as BaldiScript / PlaytimeScript).
// - Reset()'s decompile comment ("Go to X: 0, Y: 20, Z: 20") does not match its code (position
//   minus (0,20,0))  -  kept verbatim, it is the source of truth's comment.
// - Renderer type added to eng.h for the bullyRenderer field (pointer-only; hierarchy note there).
// - C# private members marked `// private (C#)`.

namespace bb {

struct BullyScript : public Component {
  virtual void Start() {
    audioDevice = GetComponent<AudioSource>(); //Get the Audio Source
    waitTime = Random::range(60.0f, 120.0f); //Set the amount of time before the bully appears again
  }

  virtual void Update(float dt) {
    if (waitTime > 0.0f) //Decrease the waittime
    {
      waitTime -= Time::deltaTime();
    }
    else if (!active)
    {
      Activate(); //Activate the Bully
    }
    if (active) //If the Bully is on the map
    {
      activeTime += Time::deltaTime(); //Increase active time
      if (activeTime >= 180.0f && (transform->position - player->position).magnitude() >= 120.0f) //If the bully has been in the map for a long time and the player is far away
      {
        Reset(); //Reset the bully
      }
    }
    if (guilt > 0.0f)
    {
      guilt -= Time::deltaTime(); //Decrease Bully's guilt
    }
  }

  virtual void FixedUpdate() {
    Vec3 direction = player->position - transform->position;
    RaycastHit raycastHit;
    if (Physics::Raycast(transform->position + Vec3(0.0f, 4.0f, 0.0f), direction, &raycastHit, std::numeric_limits<float>::infinity(), 769, QueryTriggerInteraction::Ignore) && raycastHit.transform->tag() == "Player" && (transform->position - player->position).magnitude() <= 30.0f && active)
    {
      if (!spoken) // If the bully hasn't already spoken
      {
        int num = Mathf::RoundToInt(Random::range(0.0f, 1.0f)); //Get a random number between 0 and 1
        audioDevice->PlayOneShot(aud_Taunts[num]); //Say a line in an index using num
        spoken = true; //Sets spoken to true, preventing the bully from talking again
      }
      guilt = 10.0f; //Makes the bully guilty for "Bullying in the halls"
    }
  }

  void Activate() // private (C#)
  {
    wanderer->GetNewTargetHallway(); //Get a hallway position
    transform->position = wanderTarget->position + Vec3(0.0f, 5.0f, 0.0f); // Go to the wanderTarget + 5 on the Y axis
    while ((transform->position - player->position).magnitude() < 20.0f) // While the Bully is close to the player
    {
      wanderer->GetNewTargetHallway(); //Get a new target
      transform->position = wanderTarget->position + Vec3(0.0f, 5.0f, 0.0f);// Go to the wanderTarget + 5 on the Y axis
    } //This is here to prevent the bully from spawning ontop iof the player
    active = true; //Set the bully to active
  }

  virtual void OnTriggerEnter(Collider* other) {
    if (other->transform->tag() == "Player") // If touching the player
    {
      if (gc->item[0] == 0 && gc->item[1] == 0 && gc->item[2] == 0) // If the player has no items
      {
        audioDevice->PlayOneShot(aud_Denied); // "What, no items? No Items? No passsssss"
      }
      else
      {
        int num = Mathf::RoundToInt(Random::range(0.0f, 2.0f)); //Get a random item slot
        while (gc->item[num] == 0) //If the selected slot is empty
        {
          num = Mathf::RoundToInt(Random::range(0.0f, 2.0f)); // Choose another slot
        }
        gc->LoseItem(num); // Remove the item selected
        int num2 = Mathf::RoundToInt(Random::range(0.0f, 1.0f));
        audioDevice->PlayOneShot(aud_Thanks[num2]);
        Reset();
      }
    }
  }

  virtual void OnTriggerStay(Collider* other) {
    if (other->transform->name() == "Principal of the Thing" && guilt > 0.0f) //If touching the principal and the bully is guilty
    {
      Reset(); //Reset the bully
    }
  }

  void Reset() // private (C#)
  {
    transform->position = transform->position - Vec3(0.0f, 20.0f, 0.0f); // Go to X: 0, Y: 20, Z: 20
    waitTime = Random::range(60.0f, 120.0f); //Set the amount of time before the bully appears again
    active = false; //Set active to false
    activeTime = 0.0f; //Reset active time
    spoken = false; //Reset spoken
  }

  // ============ serialized fields (C# field-block order) ============
  Transform* player = 0;
  GameControllerScript* gc = 0;
  Renderer* bullyRenderer = 0;
  Transform* wanderTarget = 0;
  AILocationSelectorScript* wanderer = 0;
  float waitTime = 0.0f;
  float activeTime = 0.0f;
  float guilt = 0.0f;
  bool active = false;
  bool spoken = false;
  AudioSource* audioDevice = 0;        // private (C#)
  AudioClip* aud_Taunts[2] = {0, 0};   // C# constructor-fixes 2
  AudioClip* aud_Thanks[2] = {0, 0};   // C# constructor-fixes 2
  AudioClip* aud_Denied = 0;
};

} // namespace bb
