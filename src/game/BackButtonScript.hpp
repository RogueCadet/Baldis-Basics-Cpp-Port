#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\Core\UI\Buttons\BackButtonScript.cs (17 lines; hides the
// bound screen when the button fires).
// - UnityAction/UnityEvent gained AddListener in eng.h; member-method listeners use a tiny adapter
//   struct mirroring C# `new UnityAction(CloseScreen)` (unified delegate wrapper).
namespace bb {

struct BackButtonScript;

// C# `new UnityAction(CloseScreen)` delegate -> adapter over the member method.
struct BackCloseScreenAction : public UnityAction {
  BackButtonScript* self;
  explicit BackCloseScreenAction(BackButtonScript* s) : self(s) {}
  void Invoke();
};

struct BackButtonScript : public Component {
  virtual void Start() {
    button = GetComponent<Button>();
    button->onClick.AddListener(new BackCloseScreenAction(this));
  }

  void CloseScreen() // private (C#)
  {
    screen->SetActive(false);
  }

  // ============ serialized fields (C# field-block order) ============
  Button* button = 0;                  // private (C#)
  GameObject* screen = 0;
};

inline void BackCloseScreenAction::Invoke() { self->CloseScreen(); }

} // namespace bb
