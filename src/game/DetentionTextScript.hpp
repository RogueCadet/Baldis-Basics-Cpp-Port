#pragma once
#include "../eng.h"
#include "DoorScript.hpp" // door.lockTime read every frame (DoorScript never references this: cycle-free)

// 1:1 translation of Assets\Scripts\PlayerFunctions\DoorFunctions\DetentionTextScript.cs (22
// lines; the "You have detention!" countdown over the locked door). TMP -> UiText convention.
// - string + int concat -> std::string + std::to_string (int formatting is parity-safe).
// - string.Empty -> std::string().
// - TMP .text writes -> UiText::SetText(...) (MathGame convention; UiText is engine-rasterized).
// - Mathf.CeilToInt added to eng.h.
namespace bb {

struct DetentionTextScript : public Component {
  virtual void Start() {
    text = GetComponent<UiText>();
  }

  virtual void Update(float dt) {
    if (door->lockTime > 0.0f)
    {
      text->SetText(std::string("You have detention! \n") + std::to_string(Mathf::CeilToInt(door->lockTime)) + " seconds remain!");
    }
    else
    {
      text->SetText(std::string());
    }
  }

  // ============ serialized fields (C# field-block order) ============
  DoorScript* door = 0;
  UiText* text = 0;                    // private (C#)  -  C# TMP_Text
};

} // namespace bb
