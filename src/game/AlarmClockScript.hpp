#pragma once
#include "../eng.h"
#include "BaldiScript.hpp" // baldi.isActiveAndEnabled / Hear in Alarm()

// 1:1 translation of Assets\Scripts\PlayerFunctions\ItemFunctions\AlarmClockScript.cs (42 lines;
// the alarm-clock item: rings after 30 s at priority 8, then self-destructs at 35 s).
// Standalone (GC wires it via GetComponent + `->baldi = baldiScrpt;` in UseItem item 7  -  the
// complete type is included in GameControllerScript.hpp).
// Engine addition: Destroy(GameObject*, float) delayed overload (Unity end-of-frame at 0f).
// C# private members marked `// private (C#)`.

namespace bb {

struct AlarmClockScript : public Component {
  virtual void Start() {
    timeLeft = 30.0f;
    lifeSpan = 35.0f;
  }

  virtual void Update(float dt) {
    if (timeLeft >= 0.0f) //If the time is greater then 0
    {
      timeLeft -= Time::deltaTime(); //Decrease the time variable
    }
    else if (!rang) // If it has not been rang
    {
      Alarm(); // Start the alarm function
    }
    if (lifeSpan >= 0.0f) //If the time left in the lifespan is greater then 0
    {
      lifeSpan -= Time::deltaTime(); //Decrease the time variable
    }
    else
    {
      Destroy(gameObject, 0.0f); //Otherwise, if time is less then 0, destroy the alarm clock
    }
  }

  void Alarm() // private (C#)
  {
    rang = true;
    if (baldi->isActiveAndEnabled()) baldi->Hear(transform->position, 8.0f); //Baldi is told to go to this location, with a priority of 10(above most sounds)
    audioDevice->clip = ring;
    audioDevice->loop = false; // Tells the audio not to loop
    audioDevice->Play(); //Play the audio
  }

  // ============ serialized fields (C# field-block order) ============
  float timeLeft = 0.0f;
  float lifeSpan = 0.0f;               // private (C#)
  bool rang = false;                   // private (C#)
  BaldiScript* baldi = 0;
  AudioClip* ring = 0;
  AudioSource* audioDevice = 0;
};

} // namespace bb
