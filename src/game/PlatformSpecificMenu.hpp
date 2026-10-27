#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\Core\PlatformSpecificMenu.cs (10 lines; the PC menu is the
// always-on variant in this build; mobile stays untouched). Engine-only.
// (The touch variant will be re-evaluated when the iOS layer lands  -  keep both fields wired.)
namespace bb {

struct PlatformSpecificMenu : public Component {
  virtual void Start() {
    pC->SetActive(true);
  }

  // ============ serialized fields (C# field-block order) ============
  GameObject* pC = 0;
  GameObject* mobile = 0;
};

} // namespace bb
