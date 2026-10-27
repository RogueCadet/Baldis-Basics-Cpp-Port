#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\EndlessTextScript.cs (16 lines; appends the "High Score:"
// line to whatever the endless-mode results label already shows).
// - string.Concat(new object[]{...}) -> std::string concatenation (int ToString -> std::to_string).
// - TMP .text read -> UiText::GetText() (interface addition; writes stay SetText per convention).
namespace bb {

struct EndlessTextScript : public Component {
  virtual void Start() {
    text->SetText(text->GetText() + std::string("\nHigh Score: ") + std::to_string(PlayerPrefs::GetInt("HighBooks")) + " Notebooks");
  }

  // ============ serialized fields (C# field-block order) ============
  UiText* text = 0;                    // C# TMP_Text
};

} // namespace bb
