#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\Core\CursorControllerScript.cs (14 lines; nothing but the
// two cursor toggles). Unity's Cursor/CursorLockMode surface was added to eng.h  -  engine impl
// does SDL relative mode on the Windows testbed and no-ops on iOS (touch: no system cursor).
namespace bb {

struct CursorControllerScript : public Component {
  void LockCursor()
  {
    Cursor::lockState = CursorLockMode::Locked; // Lock the cursor(prevent it from moving)
    Cursor::visible = false;
  }
  void UnlockCursor()
  {
    Cursor::lockState = CursorLockMode::None; // Unlock the cursor(allow it to move)
    Cursor::visible = true;
  }
};

} // namespace bb
