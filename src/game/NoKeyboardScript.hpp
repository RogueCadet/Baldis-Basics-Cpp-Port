#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\NoKeyboardScript.cs (11 lines; touch-UI input fields that
// suppress the on-screen keyboard).
// - Derives Unity InputField -> our bb::InputField (same base).
// - The decompile prints `Start();` inside the override; shipped-game semantics require that to
//   be the base call, so it is translated as InputField::Start() (our base Start is a no-op hook;
//   a recursive self-call would loop forever and cannot be what shipped).
// - TouchScreenKeyboardType enum added to eng.h (explicit (-1) cast kept).
namespace bb {

struct NoKeyboardScript : public InputField {
  virtual void Start() { // protected (C#) override
    keyboardType = (int)TouchScreenKeyboardType(-1);
    InputField::Start();               // base.Start() semantics (see note above)
  }
};

} // namespace bb
