#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\GameOverScript.cs.
// C# note: member `LoadScene` (string field) and `LoadScene()` (method) coexist in C#;
// in C++ the method is renamed `doLoadScene` (serialized field name is preserved for bindings).
// Time.deltaTime == the Update(float dt) argument supplied by the engine.
namespace bb {

struct GameOverScript : public Component {
  virtual void Start() override {
    image = GetComponent<Image>();
    audioDevice = GetComponent<AudioSource>();
    delay = 5.0f;
    chance = Random::range(1.0f, 99.0f);
    if (chance < 98.0f) {
      int num = Mathf::RoundToInt(Random::range(0.0f, 4.0f));
      image->sprite = images[num];
    } else {
      image->sprite = rare;
    }
  }

  virtual void Update(float dt) override {
    delay -= 1.0f * dt;
    if (delay <= 0.0f) {
      if (chance < 98.0f) {
        doLoadScene();
      } else {
        image->transform->scale = Vec3(5.0f, 5.0f, 1.0f);
        image->color = Color(1.0f, 0.0f, 0.0f, 1.0f); // Color.red
        if (!audioDevice->isPlaying()) audioDevice->Play();
        if (delay <= -5.0f) QuitApplication();
      }
    }
  }

  void doLoadScene() { bb::LoadScene(LoadScene); }

  Image* image = 0;              // runtime (GetComponent)
  float delay = 0;
  Sprite* images[5] = {0, 0, 0, 0, 0}; // serialized
  Sprite* rare = 0;              // serialized
  float chance = 0;
  AudioSource* audioDevice = 0;  // runtime (GetComponent)
  std::string LoadScene;         // serialized
};

} // namespace bb
