#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\Core\UI\Settings\DefaultSettingsScript.cs (23 lines; the
// options splash on very first run: shows the panel for one frame, then hides it).
// - `public IEnumerator CloseOptions()` -> Coroutine state machine (method returns the machine;
//   struct carries the C#-given name with a Cor suffix to dodge the identifier clash).
// - WaitForEndOfFrame -> our engine's end-of-frame boundary == the next engine tick, so the
//   machine yields one frame exactly like a `yield return null` in our model, then finishes via
//   `yield break` (entry statements already ran through the first tick by contract).
// - Canvas.enabled toggle; Canvas type from the GC-era eng.h.
namespace bb {

struct DefaultSettingsScript;

// private IEnumerator CloseOptions(): WaitForEndOfFrame, then panel swap + yield break.
// tick() body sits out-of-line below (it dereferences the fwd-only DefaultSettingsScript).
struct CloseOptionsCor : public Coroutine {
  DefaultSettingsScript* ds;
  int stage = 0;
  explicit CloseOptionsCor(DefaultSettingsScript* ds_) : ds(ds_) {}
  bool tick(float dt);
};

struct DefaultSettingsScript : public Component {
  virtual void Start() {
    if (!PlayerPrefs::HasKey("OptionsSet"))
    {
      options->SetActive(true);
      StartCoroutine(CloseOptions());
      canvas->enabled = false;
    }
  }

  Coroutine* CloseOptions()            // C# public IEnumerator CloseOptions()
  {
    return new CloseOptionsCor(this);
  }

  // ============ serialized fields (C# field-block order) ============
  GameObject* options = 0;
  Canvas* canvas = 0;
};

inline bool CloseOptionsCor::tick(float dt) {
  if (stage == 0)
  {
    stage = 1;
    return true;                       // yield return new WaitForEndOfFrame() -- SUSPEND (true =
                                       // still running). Returning false here finished the machine
                                       // inside StartCoroutine's synchronous prefix, so canvas was
                                       // re-enabled BEFORE the caller's `canvas->enabled = false`.
  }
  ds->canvas->enabled = true;
  ds->options->SetActive(false);
  return false;                        // yield break
}

} // namespace bb
