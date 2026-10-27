#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\Core\UI\Buttons\SecretScript.cs.
// SceneManager.LoadScene -> bb::LoadScene (names identical to original scene names).
namespace bb {

struct SecretScript : public Component {
  void LoadScene() { bb::LoadScene("Secret"); }
};

} // namespace bb
