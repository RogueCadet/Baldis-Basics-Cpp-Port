#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\ScoreScript.cs.
// TMP_Text replaced by bb::UiText (TTF-rasterized engine text, Phase 2).
// Serialized fields keep source names (scoreText, text) for the data binding pass.
namespace bb {

struct ScoreScript : public Component {
  virtual void Start() override {
    if (PlayerPrefs::GetString("CurrentMode") == "endless") {
      scoreText->SetActive(true);
      text->SetText(std::string("Score:\n") + std::to_string(PlayerPrefs::GetInt("CurrentBooks")) + " Notebooks");
    }
  }

  GameObject* scoreText = 0; // serialized
  UiText* text = 0;          // serialized (was TMP_Text)
};

} // namespace bb
