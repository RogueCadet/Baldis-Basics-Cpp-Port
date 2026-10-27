#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\Core\UI\Buttons\StartButton.cs (24 lines; story/endless mode
// picker -> "CurrentMode" pref -> async load of the target scene).
// - Nested enum Mode -> scoped inside the struct (Unity-serialized as int).
// - `LoadScene` field name kept even though it shadows nothing in C++ (it is a plain member).
namespace bb {

struct StartButton : public Component {
  enum class Mode : int { Story = 0, Endless = 1 };

  void StartGame()
  {
    if (currentMode == Mode::Story)
    {
      PlayerPrefs::SetString("CurrentMode", "story");
    }
    else
    {
      PlayerPrefs::SetString("CurrentMode", "endless");
    }
    LoadSceneAsync(LoadScene);
  }

  // ============ serialized fields (C# field-block order) ============
  Mode currentMode = Mode::Story;
  std::string LoadScene;
};

} // namespace bb
