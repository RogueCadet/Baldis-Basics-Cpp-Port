#pragma once
#include "../eng.h"
#include "AmbienceScript.hpp" // ambience->PlayAudio() below needs the complete type

// 1:1 translation of Assets\Scripts\NPCFunctions\AILocationSelectorScript.cs (24 lines).
// Note the Unity quirk: the script TELEPORTS ITSELF to the chosen AI location  -  callers (Baldi's
// Wandering, the teleporter item) then read this transform's position as the target.
// newLocation[29] size is constructor-mandated in the C# (new Transform[29]), so it is exact.
// Mathf.RoundToInt(Random.Range(a, b)) uses the FLOAT overload -> inclusive [a, b] (Unity parity).
namespace bb {

struct AILocationSelectorScript : public Component {
  void GetNewTarget()
  {
    id = Mathf::RoundToInt(Random::range(0.0f, 28.0f)); //Get a random number between 0 and 28
    transform->position = newLocation[id]->position; //Set it's location to a position in a list of positions using the ID variable that just got set.
    ambience->PlayAudio(); //Play an ambience audio
  }

  void GetNewTargetHallway()
  {
    id = Mathf::RoundToInt(Random::range(0.0f, 15.0f)); //Get a random number between 0 and 15
    transform->position = newLocation[id]->position; //Set it's location to a position in a list of positions using the ID variable that just got set.
    ambience->PlayAudio(); //Play an ambience audio
  }

  void QuarterExclusive()
  {
    id = Mathf::RoundToInt(Random::range(1.0f, 15.0f)); //Get a random number between 0 and 15
    transform->position = newLocation[id]->position; //Set it's location to a position in a list of positions using the ID variable that just got set.
  }

  // ============ serialized fields (C# field-block order) ============
  Transform* newLocation[29] = {0};    // C# constructor fixes the size (new Transform[29])
  AmbienceScript* ambience = 0;
  int id = 0;                          // private (C#)
};

} // namespace bb
