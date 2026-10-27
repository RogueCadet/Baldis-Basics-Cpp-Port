#pragma once
#include "../eng.h"
#include "StubScripts.hpp"          // InputManager stub (Singleton<InputManager>)
#include "GameControllerScript.hpp" // gc.CollectItem below (one-way: GC never references PickupScript)

// 1:1 translation of Assets\Scripts\PlayerFunctions\ItemFunctions\PickupScript.cs (72 lines; the
// world-pickup scanner: eleven named pickups, distance <= 10, deactivate + CollectItem(n)).
// Ray origin/direction conversions kept; C# `&` between side-effect-free compares -> `&&`.
// - raycastHit.transform.gameObject -> Transform::gameObject() accessor (added to eng.h).
// - Vector3.Distance -> dist(...) (bbmath, as in the other translations).
namespace bb {

struct PickupScript : public Component {
  virtual void Update(float dt) {
    // iOS: Rewired GetButtonDown("Interact") (ios-ref/classes/PickupScript.ios.c)
    if ((ApplicationIsMobilePlatform() ? ReInputPlayer().GetButtonDown("Interact") : Singleton<InputManager>::Instance()->GetActionKeyDown(InputAction::Interact)) && Time::timeScale != 0.0f)
    {
      Ray ray = Camera::main()->ScreenPointToRay(Vec3((float)(Screen::width() / 2), (float)(Screen::height() / 2), 0.0f));
      RaycastHit raycastHit;
      if (Physics::Raycast(ray, &raycastHit))
      {
        if (raycastHit.transform->name() == "Pickup_EnergyFlavoredZestyBar" && dist(player->position, transform->position) < 10.0f)
        {
          raycastHit.transform->gameObject()->SetActive(false);
          gc->CollectItem(1);
        }
        else if (raycastHit.transform->name() == "Pickup_YellowDoorLock" && dist(player->position, transform->position) < 10.0f)
        {
          raycastHit.transform->gameObject()->SetActive(false);
          gc->CollectItem(2);
        }
        else if (raycastHit.transform->name() == "Pickup_Key" && dist(player->position, transform->position) < 10.0f)
        {
          raycastHit.transform->gameObject()->SetActive(false);
          gc->CollectItem(3);
        }
        else if (raycastHit.transform->name() == "Pickup_BSODA" && dist(player->position, transform->position) < 10.0f)
        {
          raycastHit.transform->gameObject()->SetActive(false);
          gc->CollectItem(4);
        }
        else if (raycastHit.transform->name() == "Pickup_Quarter" && dist(player->position, transform->position) < 10.0f)
        {
          raycastHit.transform->gameObject()->SetActive(false);
          gc->CollectItem(5);
        }
        else if (raycastHit.transform->name() == "Pickup_Tape" && dist(player->position, transform->position) < 10.0f)
        {
          raycastHit.transform->gameObject()->SetActive(false);
          gc->CollectItem(6);
        }
        else if (raycastHit.transform->name() == "Pickup_AlarmClock" && dist(player->position, transform->position) < 10.0f)
        {
          raycastHit.transform->gameObject()->SetActive(false);
          gc->CollectItem(7);
        }
        else if (raycastHit.transform->name() == "Pickup_WD-3D" && dist(player->position, transform->position) < 10.0f)
        {
          raycastHit.transform->gameObject()->SetActive(false);
          gc->CollectItem(8);
        }
        else if (raycastHit.transform->name() == "Pickup_SafetyScissors" && dist(player->position, transform->position) < 10.0f)
        {
          raycastHit.transform->gameObject()->SetActive(false);
          gc->CollectItem(9);
        }
        else if (raycastHit.transform->name() == "Pickup_BigBoots" && dist(player->position, transform->position) < 10.0f)
        {
          raycastHit.transform->gameObject()->SetActive(false);
          gc->CollectItem(10);
        }
        else if (raycastHit.transform->name() == "Pickup_Teleporter" && dist(player->position, transform->position) < 10.0f)
        {
          raycastHit.transform->gameObject()->SetActive(false);
          gc->CollectItem(11);
        }
      }
    }
  }

  // ============ serialized fields (C# field-block order) ============
  GameControllerScript* gc = 0;
  Transform* player = 0;
};

} // namespace bb
