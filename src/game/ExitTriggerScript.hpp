#pragma once
#include "../eng.h"
#include "GameControllerScript.hpp" // gc -> complete (notebooks/failedNotebooks reads below)

// 1:1 translation of Assets\Scripts\PlayerFunctions\DoorFunctions\ExitTriggerScript.cs.
// C# bitwise `&` between side-effect-free bools -> `&&` (behavior-identical).
namespace bb {

struct ExitTriggerScript : public Component {
  virtual void OnTriggerEnter(Collider* other) override {
    if (gc->notebooks >= 7 && other->tag() == "Player")
    {
      if (gc->failedNotebooks >= 7) //If the player got all the problems wrong on all the 7 notebooks
      {
        LoadScene(SecretScene); //Go to the secret ending
      }
      else
      {
        LoadScene(ResultsScene); //Go to the win screen
      }
    }
  }

  GameControllerScript* gc = 0; // serialized
  std::string ResultsScene;     // serialized
  std::string SecretScene;      // serialized
};

} // namespace bb
