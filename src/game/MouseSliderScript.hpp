#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\Core\UI\Settings\MouseSliderScript.cs (18 lines; mouse
// sensitivity clamp-to-200-on-first-read and live save). Standalone: Slider + PlayerPrefs only.
namespace bb {

struct MouseSliderScript : public Component {
  virtual void Start() {
    if (PlayerPrefs::GetFloat("MouseSensitivity") < 100.0f)
    {
      PlayerPrefs::SetFloat("MouseSensitivity", 200.0f);
    }
    slider->value = PlayerPrefs::GetFloat("MouseSensitivity");
  }

  virtual void Update(float dt) {
    PlayerPrefs::SetFloat("MouseSensitivity", slider->value);
  }

  // ============ serialized fields (C# field-block order) ============
  Slider* slider = 0;
};

} // namespace bb
