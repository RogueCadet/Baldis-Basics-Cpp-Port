#pragma once
#include "../eng.h"
#include "StubScripts.hpp" // QuitApplication via eng (kept note: Application.Quit())

// 1:1 translation of Assets\Scripts\YouWonScript.cs (17 lines; the win screen self-quits after
// 10 s). Engine-only: QuitApplication is eng.h's Application.Quit.
namespace bb {

struct YouWonScript : public Component {
  virtual void Start() {
    delay = 10.0f;
  }

  virtual void Update(float dt) {
    delay -= Time::deltaTime();
    if (delay <= 0.0f)
    {
      QuitApplication();
    }
  }

  // ============ fields (C# field-block order) ============
  float delay = 0.0f;                  // private (C#)
};

} // namespace bb
