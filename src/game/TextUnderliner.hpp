#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\TextUnderliner.cs (14 lines; the tiny options-menu font-style
// toggler). TMP -> UiText convention: fontStyle is the TMP FontStyles mirror field (values in
// eng.h: Normal=0, Bold=1, Italic=2, Underline=4).
namespace bb {

struct TextUnderliner : public Component {
  void Underline()
  {
    text->fontStyle = (int)FontStyles::Underline;
  }

  void Ununderline()
  {
    text->fontStyle = (int)FontStyles::Normal;
  }

  // ============ serialized fields (C# field-block order) ============
  UiText* text = 0;                    // C# TMP_Text
};

} // namespace bb
