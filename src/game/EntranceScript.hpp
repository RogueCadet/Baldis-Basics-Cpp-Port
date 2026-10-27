#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\EntranceScript.cs.
//
// Include architecture: Lower() reads gc.finaleMode while GameControllerScript's
// ActivateSpoopMode/ActivateFinaleMode call es->Lower/Raise  -  one dependency runs each way,
// which C# compiles per-file but C++ headers cannot do cyclically. This struct therefore
// forward-declares GameControllerScript and carries method declarations only; all four
// out-of-line bodies live at the bottom of GameControllerScript.hpp (where both classes are
// complete). Behavior unchanged 1:1.

namespace bb {

struct GameControllerScript; // fwd (full definition incl. the out-of-line bodies: GameControllerScript.hpp)

struct EntranceScript : public Component {
  void Raise();   // body: bottom of GameControllerScript.hpp (+10 y; no finale branch)
  void Lower();   // body: bottom of GameControllerScript.hpp (-10 y; wall.material swap on finaleMode)

  GameControllerScript* gc = 0; // serialized
  Material* map = 0;            // serialized
  MeshRenderer* wall = 0;       // serialized
};

} // namespace bb
