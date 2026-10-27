#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\Core\UI\Buttons\ExitButtonScript.cs.
namespace bb {

struct ExitButtonScript : public Component {
  void ExitGame() { QuitApplication(); }
};

} // namespace bb
