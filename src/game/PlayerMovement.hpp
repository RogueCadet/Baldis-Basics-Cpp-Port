#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\PlayerFunctions\PlayerMovement.cs (63 lines; LEGACY player
// controller on stock input axes). PlayerScript.cs is the live v1.3+ controller (Rewired actions
// + guilt/jumprope/sweep integrations). This one is kept for decompile parity  -  confirm at
// Phase 1 scene binding whether any shipped scene actually has it attached (suspect dead code).
// C# bitwise `&` between side-effect-free bools -> `&&` (behavior-identical).
namespace bb {

struct PlayerMovement : public Component {
  virtual void Awake() {
    mouseSensitivity = PlayerPrefs::GetFloat("MouseSensitivity");
  }
  virtual void Start() {
    stamina = staminaMax;
    Time::timeScale = 1.0f;
  }
  virtual void Update(float dt) {
    running = Input::GetButton("Run");
    MouseMove();
    PlayerMove();
    StaminaUpdate();
  }
  void MouseMove() // private (C#)
  {
    Quat rotation = transform->rotation;
    rotation.setEulerAngles(rotation.eulerAngles() + Vec3(0.0f, 1.0f, 0.0f) * (Input::GetAxis("Mouse X") * mouseSensitivity * Time::deltaTime() * Time::timeScale));
    transform->rotation = rotation;
  }
  void PlayerMove() // private (C#)
  {
    float d = walkSpeed;
    if (stamina > 0.0f && running)
    {
      d = runSpeed;
    }
    Vec3 a = transform->right() * Input::GetAxis("Strafe");
    Vec3 b = transform->forward() * Input::GetAxis("Forward");
    sensitivity = Mathf::Clamp((a + b).magnitude(), 0.0f, 1.0f);
    cc->Move(bb::normalized(a + b) * d * sensitivity * Time::deltaTime());
  }
  void StaminaUpdate()
  {
    if (cc->velocity().magnitude() > cc->minMoveDistance)
    {
      if (running)
      {
        stamina = Mathf::Max(stamina - staminaDrop * Time::deltaTime(), 0.0f);
      }
    }
    else if (stamina < staminaMax)
    {
      stamina += staminaRise * Time::deltaTime();
    }
  }

  // ============ serialized fields (C# bottom block order) ============
  CharacterController* cc = 0;
  float walkSpeed = 0.0f;
  float runSpeed = 0.0f;
  float stamina = 0.0f;
  float staminaDrop = 0.0f;
  float staminaRise = 0.0f;
  float staminaMax = 0.0f;
  float sensitivity = 0.0f;       // private (C#)
  float mouseSensitivity = 0.0f;  // private (C#)
  bool running = false;           // private (C#)
};

} // namespace bb
