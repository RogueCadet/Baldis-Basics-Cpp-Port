#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\AudioQueueScript.cs (40 lines; the Principal's queue-then-
// fire voice line player, 100 deep). Standalone: engine-only dependencies.
namespace bb {

struct AudioQueueScript : public Component {
  virtual void Start() {
    audioDevice = GetComponent<AudioSource>();
  }

  virtual void Update(float dt) {
    if (!audioDevice->isPlaying() && audioInQueue > 0)
    {
      PlayQueue();
    }
  }

  void QueueAudio(AudioClip* sound)
  {
    audioQueue[audioInQueue] = sound;
    audioInQueue++;
  }

  void PlayQueue() // private (C#)
  {
    audioDevice->PlayOneShot(audioQueue[0]);
    UnqueueAudio();
  }

  void UnqueueAudio() // private (C#)
  {
    for (int i = 1; i < audioInQueue; i++)
    {
      audioQueue[i - 1] = audioQueue[i];
    }
    audioInQueue--;
  }

  void ClearAudioQueue()
  {
    audioInQueue = 0;
  }

  // ============ serialized/predefined fields (C# field-block order) ============
  AudioSource* audioDevice = 0;        // private (C#)
  int audioInQueue = 0;                // private (C#)
  AudioClip* audioQueue[100] = {0};    // private (C#)  -  constructor-fixes 100
};

} // namespace bb
