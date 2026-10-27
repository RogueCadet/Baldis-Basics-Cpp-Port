#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\WarningScreenScript.cs.
namespace bb {

struct WarningScreenScript : public Component {
  virtual void Update(float dt) override {
    if (Input::anyKeyDown()) bb::LoadScene(LoadScene);
  }

  std::string LoadScene; // serialized
};

} // namespace bb
