#pragma once
#include "../eng.h"
#include "CameraScript.hpp"     // cs.jumpHeight read/writes below
#include "PlayerScript.hpp"     // ps.DeactivateJumpRope below
#include "PlaytimeScript.hpp"   // playtime.audioDevice / aud_Numbers / aud_Congrats / aud_Oops

// 1:1 translation of Assets\Scripts\Core\UI\JumpRopeScript.cs (81 lines; Playtime's jump-rope
// minigame: 5 numbers  -  bounce in time or lose the streak; the rope hits ~1 s after start).
// One-way includes (none of CameraScript/PlayerScript/PlaytimeScript reference this file).
// - OnEnable hook added to Component (Unity fires it on enable; resets the round state).
// - TMP -> UiText convention; `0 + "/5"` / `jumps + "/5"` -> std::to_string + "/5".
// - The C# success path calls Stop() then PlayOneShot() twice  -  preserved verbatim.
// - cameraError-prone debug surface (ropePosition 1s countdown) is the minigame pacing, kept 1:1.
namespace bb {

struct JumpRopeScript : public Component {
  virtual void OnEnable() { // private (C#)
    jumpDelay = 1.0f;
    ropeHit = true;
    jumpStarted = false;
    jumps = 0;
    jumpCount->SetText(std::to_string(0) + "/5");
    cs->jumpHeight = 0.0f;
    playtime->audioDevice->PlayOneShot(playtime->aud_ReadyGo);
  }

  virtual void Update(float dt) {
    // -debug cheat ("make NPCs ignore you"): an open rope round stops swinging (Playtime never
    // starts new ones; her FixedUpdate/Update are gated the same way).
    if (cheatNpcIgnore()) return;
    if (jumpDelay > 0.0f) //Decrease jumpDelay countdown
    {
      jumpDelay -= Time::deltaTime();
    }
    else if (!jumpStarted) //If the jump hasn't started
    {
      jumpStarted = true; //Start the jump
      ropePosition = 1.0f; //Set the rope position to 1f
      rope->SetTrigger("ActivateJumpRope"); //Activate the jumprope
      ropeHit = false;
    }
    if (ropePosition > 0.0f)
    {
      ropePosition -= Time::deltaTime();
    }
    else if (!ropeHit) //If the player has not tried to hit the rope
    {
      RopeHit();
    }
  }

  void RopeHit() // private (C#)
  {
    ropeHit = true; //Set ropehit to true
    if (cs->jumpHeight <= 0.2f)
    {
      Fail(); //Fail
    }
    else
    {
      Success(); //Succeed
    }
    jumpStarted = false;
  }

  void Success() // private (C#)
  {
    playtime->audioDevice->Stop(); //Stop all of the lines playtime is currently speaking
    playtime->audioDevice->PlayOneShot(playtime->aud_Numbers[jumps]);
    jumps++;
    jumpCount->SetText(std::to_string(jumps) + "/5");
    jumpDelay = 0.5f;
    if (jumps >= 5) //If players complete the minigame
    {
      playtime->audioDevice->Stop(); //Stop playtime from talking
      playtime->audioDevice->PlayOneShot(playtime->aud_Congrats);
      ps->DeactivateJumpRope(); //Deactivate the jumprope
    }
  }

  void Fail() // private (C#)
  {
    jumps = 0; //Reset jumps
    jumpCount->SetText(std::to_string(jumps) + "/5");
    jumpDelay = 2.0f; //Set the jump delay to 2 seconds to allow playtime to finish her line before the rope starts again
    playtime->audioDevice->PlayOneShot(playtime->aud_Oops);
  }

  // ============ serialized fields (C# field-block order) ============
  UiText* jumpCount = 0;               // C# TMP_Text
  Animator* rope = 0;
  CameraScript* cs = 0;
  PlayerScript* ps = 0;
  PlaytimeScript* playtime = 0;
  int jumps = 0;
  float jumpDelay = 0.0f;
  float ropePosition = 0.0f;
  bool ropeHit = false;
  bool jumpStarted = false;
};

} // namespace bb
