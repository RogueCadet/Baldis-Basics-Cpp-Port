#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\NPCFunctions\Script.cs (22 lines; the decompile's generically
// named ending-audio script: plays a clip once the player enters, quits the game when it ends).
// Standalone: AudioSource + QuitApplication + Debug::Log (added to eng.h) only.
// - other.name -> other->gameObject->name: Unity Component.name IS its GameObject's name, while
//   the engine's inherited Object::name field on a component is empty (see eng.h Component).
// - C# bitwise `&` between side-effect-free bools -> `&&` (behavior-identical).
// - `played` stays private (C#).
namespace bb {

struct Script : public Component {
  virtual void Update(float dt) {
    if (!audioDevice->isPlaying() && played)
    {
      QuitApplication();
      Debug::Log("The game is closed.");
    }
  }

  virtual void OnTriggerEnter(Collider* other) {
    if (other->gameObject->name == "Player" && !played)
    {
      audioDevice->Play();
      played = true;
    }
  }

  // ============ serialized fields (C# field-block order) ============
  AudioSource* audioDevice = 0;
  bool played = false;                 // private (C#)
};

} // namespace bb
