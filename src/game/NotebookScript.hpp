#pragma once
#include "../eng.h"
#include "StubScripts.hpp"          // InputManager stub (Singleton<InputManager>)
#include "GameControllerScript.hpp" // gc.mode / gc.CollectNotebook / gc assignment below
#include "BaldiScript.hpp"          // bsc pointer pass-through
#include "MathGameScript.hpp"       // GetComponent<MathGameScript> member writes need the complete type

// 1:1 translation of Assets\Scripts\PlayerFunctions\ItemFunctions\NotebookScript.cs (51 lines; the
// collectible you-can-think-pad notebook: endless-mode respawn, then spawns the math game).
// One-way includes (GC and MathGame never reference NotebookScript) -> all bodies inline.
// - `Instantiate<GameObject>(learningGame)` -> `Instantiate(learningGame)`: Unity's generic T is
//   statically GameObject here; the new 1-arg overload clones at (0,0,0) (Unity prefab spawn).
// - raycast tag compare chain: first leg already uses && in C#; the inner `&` -> `&&`.
namespace bb {

struct NotebookScript : public Component {
  virtual void Start() {
    up = true;
  }

  virtual void Update(float dt) {
    if (gc->mode == "endless")
    {
      if (respawnTime > 0.0f)
      {
        if ((transform->position - player->position).magnitude() > 60.0f)
        {
          respawnTime -= Time::deltaTime();
        }
      }
      else if (!up)
      {
        transform->position = Vec3(transform->position.x, 4.0f, transform->position.z);
        up = true;
        audioDevice->Play();
      }
    }
    // iOS: Rewired GetButtonDown("Interact") -- a press, where PC reads the held key
    // (ios-ref/classes/NotebookScript.ios.c)
    if ((ApplicationIsMobilePlatform() ? ReInputPlayer().GetButtonDown("Interact") : Singleton<InputManager>::Instance()->GetActionKey(InputAction::Interact)) && Time::timeScale != 0.0f)
    {
      Ray ray = Camera::main()->ScreenPointToRay(Vec3((float)(Screen::width() / 2), (float)(Screen::height() / 2), 0.0f));
      RaycastHit raycastHit;
      if (Physics::Raycast(ray, &raycastHit) && (raycastHit.transform->tag() == "Notebook" && dist(player->position, transform->position) < openingDistance))
      {
        transform->position = Vec3(transform->position.x, -20.0f, transform->position.z);
        up = false;
        respawnTime = 120.0f;
        gc->CollectNotebook();
        GameObject* gameObject = Instantiate(learningGame);
        gameObject->GetComponent<MathGameScript>()->gc = gc;
        gameObject->GetComponent<MathGameScript>()->baldiScript = bsc;
        gameObject->GetComponent<MathGameScript>()->playerPosition = player->position;
      }
    }
    // HEADS-UP debug aid (BB_NOTEBOOKDBG=1): diagnose why a click misses the notebook. Keep
    // gated+temporary; remove once the interact chain is confirmed working (like BB_UIHDBG).
    if (getenv("BB_NOTEBOOKDBG")) {
      if (Time::frameCount < 5) {
        printf("[notebook] frame %d up=%d interact=%d timeScale=%.2f gc=%d player=%d mode='%s'\n",
               Time::frameCount, (int)up,
               (int)Singleton<InputManager>::Instance()->GetActionKey(InputAction::Interact),
               Time::timeScale, (int)(gc != 0), (int)(player != 0), gc->mode.c_str());
      } else if (Singleton<InputManager>::Instance()->GetActionKey(InputAction::Interact)) {
        Ray dbgRay = Camera::main()->ScreenPointToRay(Vec3((float)(Screen::width() / 2), (float)(Screen::height() / 2), 0.0f));
        RaycastHit dbgHit;
        const bool hitSomething = Physics::Raycast(dbgRay, &dbgHit);
        printf("[notebook] frame %d keyHeld ray o(%.1f,%.1f,%.1f) d(%.3f,%.3f,%.3f) hit %d tag %s dist %.1f (open %.1f) timeScale %.2f\n",
               Time::frameCount, dbgRay.origin.x, dbgRay.origin.y, dbgRay.origin.z,
               dbgRay.direction.x, dbgRay.direction.y, dbgRay.direction.z,
               (int)hitSomething, hitSomething ? dbgHit.transform->tag().c_str() : "-",
               dist(player->position, transform->position), openingDistance, Time::timeScale);
      }
    }
  }

  // ============ serialized fields (C# field-block order) ============
  float openingDistance = 0.0f;
  GameControllerScript* gc = 0;
  BaldiScript* bsc = 0;
  float respawnTime = 0.0f;
  bool up = false;
  Transform* player = 0;
  GameObject* learningGame = 0;
  AudioSource* audioDevice = 0;
};

} // namespace bb
