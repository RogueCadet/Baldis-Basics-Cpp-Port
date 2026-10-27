#pragma once
#include "../eng.h"

// iOS BUILD ONLY (no PC source): MobileController, ported from the genuine iOS binary
// (ios-ref/classes/MobileController.ios.c; fields simpleControls@24 proControls@32 active@40).
// Start shows the simple touch layout; every Update re-shows it while InputTypeManager.usingTouch
// holds and hides both layouts once it clears -- so the controls appear on the first touch.
namespace bb {

struct MobileController : public Component {
  virtual void Start() { ActivateMobileControls(); }

  virtual void Update(float dt) {
    if (InputTypeManagerUsingTouch()) ActivateMobileControls();
    else if (active) DeactivateMobileControls();
  }

  void ActivateMobileControls() {
    simpleControls->SetActive(true);
    active = true;
  }

  void DeactivateMobileControls() {
    proControls->SetActive(false);
    simpleControls->SetActive(false);
    active = false;
  }

  GameObject* simpleControls = 0;
  GameObject* proControls = 0;
  bool active = false;
};

} // namespace bb
