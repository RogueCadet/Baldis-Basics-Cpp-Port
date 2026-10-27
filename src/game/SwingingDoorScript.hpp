#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\PlayerFunctions\DoorFunctions\SwingingDoorScript.cs (97
// lines; the swinging hall doors: unlocked from notebook 2 on, freezable by the BSODA, shouts
// the player's openings to Baldi unless silenced).
//
// Include architecture (mutual dependency with GameControllerScript):
//   GameControllerScript::UseItem (item 2) calls GetComponent<SwingingDoorScript>()->LockDoor;
//   this script reads gc.notebooks + baldi.Hear in Update/OnTriggerEnter. GameControllerScript.hpp
//   includes THIS header BEFORE the GC struct; the two gc-reading methods are declared here and
//   DEFINED out-of-line at the bottom of GameControllerScript.hpp where both classes are complete.
//   BaldiScript needs only a fwd here (its members appear solely in the deferred bodies).
//
// Translation notes (fidelity):
// - `&` chains of side-effect-free bools -> `&&` (identical semantics).
// - MeshCollider (barrier/trigger) added to eng.h; MeshRenderer::material swaps as in the finale.
// - `baldiDoor` clip is serialized but unused in the C# code  -  kept.
// - C# private Unity messages (Update) stay public overrides; private members marked.

namespace bb {

struct GameControllerScript; // fwd (full definition + deferred bodies: GameControllerScript.hpp)
struct BaldiScript;          // fwd (pointer field; Hear/isActiveAndEnabled used in the deferred body only)

struct SwingingDoorScript : public Component {
  virtual void Start() {
    myAudio = GetComponent<AudioSource>();
    bDoorLocked = true;
  }

  virtual void Update(float dt);    // body: bottom of GameControllerScript.hpp (reads gc.notebooks)

  virtual void OnTriggerStay(Collider* other) { // private (C#)
    if (!bDoorLocked)
    {
      bDoorOpen = true;
      inside->material = open;
      outside->material = openOutside;
      openTime = 2.0f;
    }
  }

  virtual void OnTriggerEnter(Collider* other); // body: bottom of GameControllerScript.hpp (gc.notebooks + baldi.Hear)

  void LockDoor(float time)
  {
    barrier->enabled = true;
    obstacle->SetActive(true);
    bDoorLocked = true;
    lockTime = time;
    inside->material = locked;
    outside->material = lockedOutside;
  }

  void UnlockDoor() // private (C#)
  {
    barrier->enabled = false;
    obstacle->SetActive(false);
    bDoorLocked = false;
    inside->material = closed;
    outside->material = closedOutside;
  }

  // ============ serialized fields (C# field-block order) ============
  GameControllerScript* gc = 0;
  BaldiScript* baldi = 0;
  MeshCollider* barrier = 0;
  GameObject* obstacle = 0;
  MeshCollider* trigger = 0;
  MeshRenderer* inside = 0;
  MeshRenderer* outside = 0;
  Material* closed = 0;
  Material* open = 0;
  Material* locked = 0;
  Material* closedOutside = 0;
  Material* openOutside = 0;
  Material* lockedOutside = 0;
  AudioClip* doorOpen = 0;
  AudioClip* baldiDoor = 0;            // serialized, unused in C# code
  float openTime = 0.0f;               // private (C#)
  float lockTime = 0.0f;               // private (C#)
  bool bDoorOpen = false;
  bool bDoorLocked = false;
  bool requirementMet = false;         // private (C#)
  AudioSource* myAudio = 0;            // private (C#)
};

} // namespace bb
