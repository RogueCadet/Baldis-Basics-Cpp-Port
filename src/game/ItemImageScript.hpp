#pragma once
#include "../eng.h"
#include "GameControllerScript.hpp"

// iOS BUILD ONLY (no PC source): ItemImageScript, ported from the genuine iOS binary
// (ios-ref/classes/ItemImageScript.ios.c; fields sprite@24 noItemSprite@32 blankSprite@40 gs@48;
// gs+276 = itemSelected, gs+288 = itemSlot). The touch Item button shows the selected slot's icon,
// or noItemSprite when that slot is empty (its texture is the blank one) or there is no controller.
namespace bb {

struct ItemImageScript : public Component {
  // `gs` is serialized as a reference into the iOS School scene (level2 object 21192 = the
  // GameControllerScript on 'GameController', checked in the iOS data). The prefab cannot carry a
  // cross-scene id, so it is resolved to that same single instance here.
  virtual void Start() {
    if (gs == 0) gs = FindObjectOfType<GameControllerScript>();
  }

  virtual void Update(float dt) {
    if (gs == 0) {
      sprite->texture = noItemSprite;
      return;
    }
    Texture* tex = gs->itemSlot[gs->itemSelected]->texture;
    if (tex == blankSprite) sprite->texture = noItemSprite;
    else sprite->texture = tex;
  }

  RawImage* sprite = 0;
  Texture* noItemSprite = 0;
  Texture* blankSprite = 0;
  GameControllerScript* gs = 0;
};

} // namespace bb
