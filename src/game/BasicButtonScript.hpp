#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\Core\UI\Buttons\BasicButtonScript.cs (17 lines; shows the
// bound screen when the button fires). Same UnityAction adapter convention as BackButtonScript.
namespace bb {

struct BasicButtonScript;

// C# `new UnityAction(OpenScreen)` delegate -> adapter over the member method.
struct BasicOpenScreenAction : public UnityAction {
  BasicButtonScript* self;
  explicit BasicOpenScreenAction(BasicButtonScript* s) : self(s) {}
  void Invoke();
};

struct BasicButtonScript : public Component {
  virtual void Start() {
    button = GetComponent<Button>();
    button->onClick.AddListener(new BasicOpenScreenAction(this));
  }

  void OpenScreen() // private (C#)
  {
    screen->SetActive(true);
  }

  // ============ serialized fields (C# field-block order) ============
  Button* button = 0;                  // private (C#)
  GameObject* screen = 0;
};

inline void BasicOpenScreenAction::Invoke() { self->OpenScreen(); }

} // namespace bb
