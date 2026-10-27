#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\Core\UI\Settings\OptionsManager.cs (55 lines; the options
// screen live-saves mouse sensitivity / rumble / analog move every frame, gated on "OptionsSet").
// Standalone: PlayerPrefs gained SetFloat + HasKey in eng.h for this file.
namespace bb {

struct OptionsManager : public Component {
  virtual void Start() {
    if (PlayerPrefs::HasKey("OptionsSet"))
    {
      slider->value = PlayerPrefs::GetFloat("MouseSensitivity");
      if (PlayerPrefs::GetInt("Rumble") == 1)
      {
        rumble->isOn = true;
      }
      else
      {
        rumble->isOn = false;
      }
      if (PlayerPrefs::GetInt("AnalogMove") == 1)
      {
        analog->isOn = true;
      }
      else
      {
        analog->isOn = false;
      }
    }
    else
    {
      PlayerPrefs::SetInt("OptionsSet", 1);
    }
  }

  virtual void Update(float dt) {
    PlayerPrefs::SetFloat("MouseSensitivity", slider->value);
    if (rumble->isOn)
    {
      PlayerPrefs::SetInt("Rumble", 1);
    }
    else
    {
      PlayerPrefs::SetInt("Rumble", 0);
    }
    if (analog->isOn)
    {
      PlayerPrefs::SetInt("AnalogMove", 1);
    }
    else
    {
      PlayerPrefs::SetInt("AnalogMove", 0);
    }
  }

  // ============ serialized fields (C# field-block order) ============
  Slider* slider = 0;
  Toggle* rumble = 0;
  Toggle* analog = 0;
};

} // namespace bb
