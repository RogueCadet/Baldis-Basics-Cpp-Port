#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\Core\UiSettings.cs (87 lines; the five-size / three-height
// radio groups) -> PlayerPrefs "UiSize" / "UiHeight"). Standalone: engine + ui type only.
// Toggle type added to eng.h (Unity Toggle.isOn). C# private members marked.
namespace bb {

struct UiSettings : public Component {
  void UpdateState()
  {
    if (sAuto->isOn)
    {
      PlayerPrefs::SetInt("UiSize", 0);
    }
    else if (sXLarge->isOn)
    {
      PlayerPrefs::SetInt("UiSize", 1);
    }
    else if (sLarge->isOn)
    {
      PlayerPrefs::SetInt("UiSize", 2);
    }
    else if (sMed->isOn)
    {
      PlayerPrefs::SetInt("UiSize", 3);
    }
    else if (sSmall->isOn)
    {
      PlayerPrefs::SetInt("UiSize", 4);
    }
    if (hLow->isOn)
    {
      PlayerPrefs::SetInt("UiHeight", 0);
    }
    else if (hMed->isOn)
    {
      PlayerPrefs::SetInt("UiHeight", 1);
    }
    else if (hHigh->isOn)
    {
      PlayerPrefs::SetInt("UiHeight", 2);
    }
  }

  void RestoreState()
  {
    size = PlayerPrefs::GetInt("UiSize");
    height = PlayerPrefs::GetInt("UiHeight");
    if (size == 0)
    {
      sAuto->isOn = true;
    }
    else if (size == 1)
    {
      sXLarge->isOn = true;
    }
    else if (size == 2)
    {
      sLarge->isOn = true;
    }
    else if (size == 3)
    {
      sMed->isOn = true;
    }
    else if (size == 4)
    {
      sSmall->isOn = true;
    }
    if (height == 0)
    {
      hLow->isOn = true;
    }
    else if (height == 1)
    {
      hMed->isOn = true;
    }
    else if (height == 2)
    {
      hHigh->isOn = true;
    }
  }

  // ============ serialized fields (C# field-block order) ============
  Toggle* sAuto = 0;
  Toggle* sXLarge = 0;
  Toggle* sLarge = 0;
  Toggle* sMed = 0;
  Toggle* sSmall = 0;
  Toggle* hLow = 0;
  Toggle* hMed = 0;
  Toggle* hHigh = 0;
  int size = 0;                        // private (C#)
  int height = 0;                      // private (C#)
};

} // namespace bb
