#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\Core\UI\Settings\UiManager.cs (44 lines; applies the saved
// "UiSize"/"UiHeight" prefs to the menu layout: scaler reference resolutions + screen-eighth
// shifts for the whole UI group). RectTransform gained a position member; CanvasScaler /
// DpCanvasScaler (MaterialKit) added to eng.h.
// - C# `RectTransform[] transforms` foreach -> fixed-capacity array bound by sizeof/sizeof; the
//   Phase 1 scene binding must fill exactly the bound slots (same convention as Ambience sounds[]).
// - @int decompile naming -> `int_` free locals (keywords-safe).
namespace bb {

struct UiManager : public Component {
  // The iOS build runs this in Awake (PC: Start) and adds preset 5 (1600x720) --
  // ios-ref/classes/UiManager.ios.c (reference resolutions decoded from the float constants).
  virtual void Awake() { if (ApplicationIsMobilePlatform()) Apply(); }
  virtual void Start() { if (!ApplicationIsMobilePlatform()) Apply(); }

  void Apply() {
    int int_ = PlayerPrefs::GetInt("UiSize");
    int int2 = PlayerPrefs::GetInt("UiHeight");
    if (int_ == 1)
    {
      normScaler->referenceResolution = Vec2(640.0f, 480.0f);
    }
    else if (int_ == 2)
    {
      normScaler->referenceResolution = Vec2(800.0f, 600.0f);
    }
    else if (int_ == 3)
    {
      normScaler->referenceResolution = Vec2(900.0f, 720.0f);
    }
    else if (int_ == 4)
    {
      normScaler->referenceResolution = Vec2(1024.0f, 720.0f);
    }
    else if (int_ == 5 && ApplicationIsMobilePlatform())
    {
      normScaler->referenceResolution = Vec2(1600.0f, 720.0f);
    }
    if (int2 == 1)
    {
      for (int i = 0; i < (int)(sizeof(transforms) / sizeof(transforms[0])); i++)
      {
        RectTransform* rectTransform = transforms[i];
        rectTransform->position = Vec3(rectTransform->position.x, rectTransform->position.y + (float)(Screen::height() / 8), rectTransform->position.z);
      }
    }
    else if (int2 == 2)
    {
      for (int i = 0; i < (int)(sizeof(transforms) / sizeof(transforms[0])); i++)
      {
        RectTransform* rectTransform2 = transforms[i];
        rectTransform2->position = Vec3(rectTransform2->position.x, rectTransform2->position.y + (float)(Screen::height() / 4), rectTransform2->position.z);
      }
    }
  }

  // ============ serialized fields (C# field-block order) ============
  CanvasScaler* normScaler = 0;
  DpCanvasScaler* dpiScaler = 0;
  RectTransform* transforms[64] = {0}; // Unity-serialized size; capacity fixed until Phase 1 binding
};

} // namespace bb
