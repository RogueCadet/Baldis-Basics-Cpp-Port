#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\NPCFunctions\ClickableTest.cs (24 lines; the TestRoom's
// demo notebook: left-click it while within 10 units of the player and it plays the pickup sound
// and vanishes). The baked component ships m_Enabled=0 (the loader honors that), so it is inert
// unless something enables it -- kept for decompile parity either way.
//
// Translation notes: C# `A & B` between side-effect-free bools -> `&&` (convention); no
// maxDistance/layerMask on the Unity call -> Physics::Raycast's 2-arg overload (all layers,
// infinity). Input.mousePosition is the NEW engine field (bottom-left origin, Unity pixels).
namespace bb {

struct ClickableTest : public Component {
  virtual void Start()
  {
    GameObject* playerObject = GameObject::FindGameObjectWithTag("Player");

    if (playerObject != 0)
    {
      player = playerObject->transform;
    }
  }
  virtual void Update(float dt)
  {
    if (Input::GetMouseButtonDown(0)) //Left click
    {
      Ray ray = Camera::main()->ScreenPointToRay(Input::mousePosition());
      RaycastHit raycastHit;
      if (Physics::Raycast(ray, &raycastHit) && (raycastHit.transform->tag() == "Notebook" && (player->position - transform->position).magnitude() < 10.0f)) // If you are looking at a notebook
      {
        test->PlayOneShot(testHit);
        gameObject->SetActive(false); //Disable the notebook
      }
    }
  }

  // ============ serialized fields (C# field-block order) ============
  Transform* player = 0; // private (C#)
  AudioSource* test = 0;
  AudioClip* testHit = 0;
};

} // namespace bb
