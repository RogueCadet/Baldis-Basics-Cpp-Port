#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\AmbienceScript.cs (17 lines; the 1-in-50 ambient scare).
// - sounds[] is Unity-serialized sized per scene; bound at Phase 1 (fixed capacity keeps the
//   translation mechanical). The C# sounds.Length read derives from the fixed capacity
//   (sizeof/sizeof), which stays correct as long as the binding fills exactly the capacity
//   slots and zeroes the rest.
// - C# bitwise `&` between side-effect-free bools -> `&&` (behavior-identical).
namespace bb {

struct AmbienceScript : public Component {
  void PlayAudio()
  {
    int num = Mathf::RoundToInt(Random::range(0.0f, 49.0f));
    if (!audioDevice->isPlaying() && num == 0) // If it is not currently playing an audio device, and num is equal to 0 (1/50 chance)
    {
      transform->position = aiLocation->position; // Go to the location of the AILocation object
      int num2 = Mathf::RoundToInt(Random::range(0.0f, (float)(sizeof(sounds) / sizeof(sounds[0]) - 1))); // Choose a random number for playing a sound
      audioDevice->PlayOneShot(sounds[num2]); // Play the sound
    }
  }

  // ============ serialized fields (C# field-block order) ============
  Transform* aiLocation = 0;
  AudioClip* sounds[50] = {0};         // Unity-serialized size; capacity fixed until Phase 1 scene binding
  AudioSource* audioDevice = 0;
};

} // namespace bb
