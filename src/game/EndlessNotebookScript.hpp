#pragma once
#include "../eng.h"
#include "GameControllerScript.hpp" // Find + GetComponent<GameControllerScript> + CollectNotebook below

// 1:1 translation of Assets\Scripts\PlayerFunctions\ItemFunctions\EndlessNotebookScript.cs (27
// lines; the endless-mode notebook hub that re-arms the learning game instead of respawning).
// GameObject.Find wired dynamically at Start (engine impl Phase 2 scene lookup); C# `&` between
// side-effect-free compares -> `&&`.
namespace bb {

struct EndlessNotebookScript : public Component {
  virtual void Start() {
    gc = GameObject::Find("Game Controller")->GetComponent<GameControllerScript>(); //Find the game controller object
    player = GameObject::Find("Player")->GetComponent<Transform>(); //Find the player object
  }

  virtual void Update(float dt) {
    // iOS: Rewired GetButtonDown("Interact") (ios-ref/classes/EndlessNotebookScript.ios.c)
    if (ApplicationIsMobilePlatform() ? ReInputPlayer().GetButtonDown("Interact") : Input::GetMouseButtonDown(0)) //If left clicked
    {
      Ray ray = Camera::main()->ScreenPointToRay(Vec3((float)(Screen::width() / 2), (float)(Screen::height() / 2), 0.0f));
      RaycastHit raycastHit;
      if (Physics::Raycast(ray, &raycastHit) && (raycastHit.transform->tag() == "Notebook" && dist(player->position, transform->position) < openingDistance)) //If it is a notebook
      {
        gameObject->SetActive(false); //Disable the object being clicked
        gc->CollectNotebook(); //Collect the notebook
        learningGame->SetActive(true); //Activate the learning game
      }
    }
  }

  // ============ serialized fields (C# field-block order) ============
  float openingDistance = 0.0f;
  GameControllerScript* gc = 0;
  Transform* player = 0;
  GameObject* learningGame = 0;
};

} // namespace bb
