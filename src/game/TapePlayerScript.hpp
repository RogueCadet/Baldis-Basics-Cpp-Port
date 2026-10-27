#pragma once
#include "../eng.h"
#include "BaldiScript.hpp" // baldi.antiHearingTime / ActivateAntiHearing below

// 1:1 translation of Assets\Scripts\PlayerFunctions\ItemFunctions\TapePlayerScript.cs (29 lines;
// the tape player: pauses with the game clock, promotes Baldi's anti-hearing tape gameplay).
// Standalone (GC consumes it via GetComponent, which needs the complete type  -  wired in
// GameControllerScript.hpp). Engine additions: AudioSource::Pause/UnPause, SpriteRenderer type.
// C# bitwise `&` between side-effect-free bools -> `&&`.
// C# private members marked `// private (C#)`.

namespace bb {

struct TapePlayerScript : public Component {
  virtual void Start() {
    audioDevice = GetComponent<AudioSource>();
  }

  virtual void Update(float dt) {
    if (audioDevice->isPlaying() && Time::timeScale == 0.0f)
    {
      audioDevice->Pause();
    }
    else if (Time::timeScale > 0.0f && baldi->antiHearingTime > 0.0f)
    {
      audioDevice->UnPause();
    }
  }

  void Play()
  {
    sprite->sprite = closedSprite;
    audioDevice->Play();
    if (baldi->isActiveAndEnabled()) baldi->ActivateAntiHearing(30.0f);
  }

  // ============ serialized fields (C# field-block order) ============
  Sprite* closedSprite = 0;
  SpriteRenderer* sprite = 0;
  BaldiScript* baldi = 0;
  AudioSource* audioDevice = 0;        // private (C#)
};

} // namespace bb
