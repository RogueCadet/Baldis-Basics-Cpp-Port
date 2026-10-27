#pragma once
#include "../eng.h"
#include "BaldiScript.hpp" // baldi.isActiveAndEnabled / Hear below

// 1:1 translation of Assets\Scripts\PlayerFunctions\ItemFunctions\TapePlayerRecordingsScript.cs
// (22 lines; the tape recorder overlay: random 1-in-5 recording, priority-4 shout to Baldi).
// Standalone: BaldiScript-only game dependency, direct include (no cycles).
// - `UnityEngine.Random.Range(0f, 4f)`  -  fully-qualified form of the same float overload ->
//   Random::range(0,4) inclusive [0..4], recordings[5] exact.
// - C# private members marked `// private (C#)`.
namespace bb {

struct TapePlayerRecordingsScript : public Component {
  virtual void Start() {
    audioDevice = GetComponent<AudioSource>();
  }

  void Play()
  {
    sprite->sprite = closedSprite;
    audVal = Mathf::RoundToInt(Random::range(0.0f, 4.0f));
    audioDevice->PlayOneShot(recordings[audVal]);
    if (baldi->isActiveAndEnabled()) baldi->Hear(transform->position, 4.0f);
  }

  // ============ serialized fields (C# field-block order) ============
  Sprite* closedSprite = 0;
  SpriteRenderer* sprite = 0;
  int audVal = 0;                      // private (C#)
  AudioClip* recordings[5] = {0};      // C# constructor-fixes 5
  BaldiScript* baldi = 0;
  AudioSource* audioDevice = 0;        // private (C#)
};

} // namespace bb
