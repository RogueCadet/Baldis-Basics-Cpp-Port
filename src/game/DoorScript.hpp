#pragma once
#include "../eng.h"
#include "StubScripts.hpp"   // InputManager stub (Singleton<InputManager>) until the settings batch
#include "BaldiScript.hpp"   // baldi.isActiveAndEnabled / Hear below

// 1:1 translation of Assets\Scripts\PlayerFunctions\DoorFunctions\DoorScript.cs (112 lines; the
// regular door: opens on click or NPC walk-through, closes after 3 s, Baldi hears non-silent
// opens, Principal's Keys give 4 silent opens).
//
// Translation notes (fidelity):
// - NO GameControllerScript reference (unlike its swinging sibling)  -  one-way include from GC's
//   UseItem (items 3/8), which needs the complete type (fixed in GameControllerScript.hpp).
// - C# get-only property `DoorLocked` became a method  -  GC's item-3 check calls DoorLocked().
// - InputAction.Interact added to types.h (this file's only user).
// - Mixed `&`/`&&` preserved as written in C#: the first raycast check already uses && there;
//   the remaining side-effect-free `&` chains -> `&&`.
// - CompareTag("NPC") -> Component::CompareTag (eng.h inline).
// - The decompile comment on openTime says "decrease lockTime" twice  -  kept verbatim.
// - C# private members marked `// private (C#)`.

namespace bb {

struct DoorScript : public Component {
  virtual void Start() {
    myAudio = GetComponent<AudioSource>();
  }

  virtual void Update(float dt) {
    if (lockTime > 0.0f) // If the lock time is greater then 0, decrease lockTime
    {
      lockTime -= 1.0f * Time::deltaTime();
    }
    else if (bDoorLocked) //If the door is locked, unlock it
    {
      UnlockDoor();
    }
    if (openTime > 0.0f) // If the open time is greater then 0, decrease lockTime Decrease open time
    {
      openTime -= 1.0f * Time::deltaTime();
    }
    if (openTime <= 0.0f && bDoorOpen)
    {
      barrier->enabled = true; // Turn on collision
      invisibleBarrier->enabled = true; //Enable the invisible barrier
      bDoorOpen = false; //Set the door open status to false
      inside->material = closed; // Change one side of the door to the closed material
      outside->material = closedOutside; // Change the other side of the door to the closed material
      if (silentOpens <= 0) //If the door isn't silent
      {
        myAudio->PlayOneShot(doorClose, 1.0f); //Play the door close sound
      }
    }
    // iOS: Rewired GetButtonDown("Interact") (ios-ref/classes/DoorScript.ios.c)
    if ((ApplicationIsMobilePlatform() ? ReInputPlayer().GetButtonDown("Interact") : Singleton<InputManager>::Instance()->GetActionKeyDown(InputAction::Interact)) && Time::timeScale != 0.0f) //If the door is left clicked and the game isn't paused
    {
      Ray ray = Camera::main()->ScreenPointToRay(Vec3((float)(Screen::width() / 2), (float)(Screen::height() / 2), 0.0f));
      RaycastHit raycastHit;
      if (Physics::Raycast(ray, &raycastHit) && (raycastHit.collider == trigger && dist(player->position, transform->position) < openingDistance && !bDoorLocked))
      {
        if (baldi->isActiveAndEnabled() && silentOpens <= 0)
        {
          baldi->Hear(transform->position, 1.0f); //If the door isn't silent, Baldi hears the door with a priority of 1.
        }
        OpenDoor();
        if (silentOpens > 0) //If the door is silent
        {
          silentOpens--; //Decrease the amount of opens the door will stay quite for.
        }
      }
    }
  }

  void OpenDoor()
  {
    if (silentOpens <= 0 && !bDoorOpen) //Play the door sound if the door isn't silent
    {
      myAudio->PlayOneShot(doorOpen, 1.0f);
    }
    barrier->enabled = false; //Disable the Barrier
    invisibleBarrier->enabled = false;//Disable the invisible Barrier
    bDoorOpen = true; //Set the door open status to false
    inside->material = open; //Change one side of the door to the open material
    outside->material = openOutside; //Change the other side of the door to the open material
    openTime = 3.0f; //Set the open time to 3 seconds
  }

  virtual void OnTriggerStay(Collider* other) { // private (C#)
    if (!bDoorLocked && other->CompareTag("NPC")) //Open the door if it isn't locked and it is an NPC
    {
      OpenDoor();
    }
  }

  void LockDoor(float time) //Lock the door for a specified amount of time
  {
    bDoorLocked = true;
    lockTime = time;
  }

  void UnlockDoor() //Unlock the door
  {
    bDoorLocked = false;
  }

  bool DoorLocked() const // C# get-only property -> method
  {
    return bDoorLocked;
  }

  void SilenceDoor() //Set the amount of times the door can be open silently
  {
    silentOpens = 4;
  }

  // ============ serialized fields (C# field-block order) ============
  float openingDistance = 0.0f;
  Transform* player = 0;
  BaldiScript* baldi = 0;
  MeshCollider* barrier = 0;
  MeshCollider* trigger = 0;
  MeshCollider* invisibleBarrier = 0;
  MeshRenderer* inside = 0;
  MeshRenderer* outside = 0;
  AudioClip* doorOpen = 0;
  AudioClip* doorClose = 0;
  Material* closed = 0;
  Material* open = 0;
  Material* closedOutside = 0;
  Material* openOutside = 0;
  bool bDoorOpen = false;              // private (C#)
  bool bDoorLocked = false;            // private (C#)
  int silentOpens = 0;
  float openTime = 0.0f;               // private (C#)
  float lockTime = 0.0f;
  AudioSource* myAudio = 0;            // private (C#)
};

} // namespace bb
