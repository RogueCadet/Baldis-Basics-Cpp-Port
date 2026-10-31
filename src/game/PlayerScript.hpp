#pragma once
#include "../eng.h"
#include "StubScripts.hpp"
#include "BaldiScript.hpp" // serialized `baldi` field (pointer only; real file, no cycle)
#include "DoorScript.hpp"  // OnTriggerExit reads door->lockTime (inline body here)

// 1:1 translation of Assets\Scripts\PlayerFunctions\PlayerScript.cs (~266 lines; the LIVE player
// controller: Rewired move actions, mouse-look, stamina, guilt system, jumprope/sweep/hug states).
//
// Include architecture (mutual dependency with GameControllerScript):
//   PlayerScript::Update + ::OnTriggerEnter read gc members; GameControllerScript's Update /
//   UseItem / Teleport / Fliparoo / DeactivateLearningGame touch player members inline. C++
//   headers cannot be cyclic, so: GC gets the FULL definition here (fwd + fields + inline bodies),
//   while PlayerScript::Update and ::OnTriggerEnter are declared here and DEFINED out-of-line at
//   the bottom of GameControllerScript.hpp. Coroutine bodies (KeepTheHudOff / BootTimer) touch
//   only PlayerScript members, so they live here (below the class).
//
// Translation notes (fidelity):
// - InputAction enum mirrors the Rewired actions used (MoveForward/Backward/Left/Right/Run).
// - C# bitwise `&` between side-effect-free bools -> `&&` (behavior-identical).
// - Vector3.normalized -> bb::normalized(); .magnitude -> Vec3::magnitude().
// - RectTransform easing: none  -  playerRotation is a Quat value mirrored from transform.rotation;
//   eulerAngles reads/writes route through Quat::eulerAngles()/setEulerAngles() (engine impl,
//   Phase 2, verified against Unity parity in the testbed).
// - GetComponent<InputManager> pattern -> Singleton<InputManager>::Instance() (Singleton.hpp).
// - `height` is the CharacterController floor offset (set once in Start, used by teleport/hug).
// - RandomForest: cc.velocity is read per frame  -  engine impl pre-populates it in Phase 3.
// - C# private methods/fields marked `// private (C#)`.

namespace bb {

struct GameControllerScript; // fwd (full definition incl. out-of-line bodies: GameControllerScript.hpp)
// -debug "infinite stamina": PlayerScript::StaminaCheck early-returns on it. Defined out-of-line
// (bottom of GameControllerScript.hpp) so the C++11 library never needs the complete type here.
bool dbgStaminaPinned(const GameControllerScript* gc);
struct PlayerScript;         // fwd (defined below; the coroutine structs hold back-pointers)
struct PlaytimeScript;       // fwd (pointer field below; real: PlaytimeScript.hpp, complete where
                             // the out-of-line OnTriggerEnter body needs it)

// ---- Unity IEnumerator -> per-frame state machines -------------------------------------------

// public IEnumerator KeepTheHudOff(): hides the HUD while gameOver holds (per-frame tick).
struct KeepTheHudOff : Coroutine {
  PlayerScript* player = 0;
  explicit KeepTheHudOff(PlayerScript* p) : player(p) {}
  bool tick(float dt); // defined below the class
};

// private IEnumerator BootTimer(): 15 s timer, then bootsActive=false.
struct BootTimer : Coroutine {
  PlayerScript* player = 0;
  float time = 15.0f;
  explicit BootTimer(PlayerScript* p) : player(p) {}
  bool tick(float dt); // defined below the class
};

struct PlayerScript : public Component {
  // ============ MonoBehaviour messages ============

  virtual void Start() {
    if (PlayerPrefs::GetInt("AnalogMove") == 1)
    {
      sensitivityActive = true;
    }
    height = transform->position.y;
    stamina = maxStamina;
    playerRotation = transform->rotation;
    mouseSensitivity = PlayerPrefs::GetFloat("MouseSensitivity");
    principalBugFixer = 1;
    flipaturn = 1.0f;
  }

  virtual void Update(float dt);    // body: bottom of GameControllerScript.hpp (needs gc complete)

  void MouseMove() // private (C#)
  {
    playerRotation.setEulerAngles(Vec3(playerRotation.eulerAngles().x, playerRotation.eulerAngles().y, fliparoo));
    if (ApplicationIsMobilePlatform())
    {
      // iOS build (PlayerScript.MouseMove @0x100aaaae4; operands from the disassembly: +0x80
      // mouseSensitivity, +0x4C flipaturn, constant 200.0 @0x1019BB798). Rewired "Turn":
      // Relative -> up * axis * sens * timeScale * flipaturn; Absolute -> the same * deltaTime *
      // 200. The touch pad's LookX axis is Absolute (HardwareAxisInfo._dataFormat 0), so on iOS a
      // swipe turns by (hundredths of an inch moved) * sens * dt * 200.
      const float axis = ReInputPlayer().GetAxis("Turn");
      float turn = axis * mouseSensitivity * Time::timeScale * flipaturn;
      if (ReInputPlayer().GetAxisCoordinateMode("Turn") != AxisCoordinateMode::Relative)
        turn *= Time::deltaTime() * 200.0f;
      playerRotation.setEulerAngles(playerRotation.eulerAngles() + Vec3(0.0f, 1.0f, 0.0f) * turn);
    }
    else
    {
      playerRotation.setEulerAngles(playerRotation.eulerAngles() + Vec3(0.0f, 1.0f, 0.0f) * (Input::GetAxis("Mouse X") * mouseSensitivity * Time::timeScale * flipaturn));
    }
    transform->rotation = playerRotation;
  }

  void PlayerMove() // private (C#)
  {
    // C#: Vector3 movement = Vector3.zero; Vector3 lateralMovement = Vector3.zero;
    Vec3 movement = Vec3::zero();
    Vec3 lateralMovement = Vec3::zero();
    const bool mobile = ApplicationIsMobilePlatform();
    if (mobile)
    {
      // iOS build (PlayerScript.PlayerMove @0x100aaadbc): ANALOG Rewired axes, so the joystick's
      // deflection scales speed through `sensitivity` below.
      movement = transform->forward() * ReInputPlayer().GetAxis("Forward");
      lateralMovement = transform->right() * ReInputPlayer().GetAxis("Strafe");
    }
    else
    {
      if (Singleton<InputManager>::Instance()->GetActionKey(InputAction::MoveForward)) movement = transform->forward();
      if (Singleton<InputManager>::Instance()->GetActionKey(InputAction::MoveBackward)) movement = -transform->forward();
      if (Singleton<InputManager>::Instance()->GetActionKey(InputAction::MoveLeft)) lateralMovement = -transform->right();
      if (Singleton<InputManager>::Instance()->GetActionKey(InputAction::MoveRight)) lateralMovement = transform->right();
    }
    if (stamina > 0.0f)
    {
      if (mobile ? ReInputPlayer().GetButton("Run") : Singleton<InputManager>::Instance()->GetActionKey(InputAction::Run))
      {
        playerSpeed = runSpeed;
        sensitivity = 1.0f;
        // iOS inlines ResetGuilt and checks only !sweeping (no !hugging): `velocity > 0.1 &&
        // !sweeping && guilt <= 0.1 -> guilt = 0.1, guiltType = "running"` -- same effect as
        // ResetGuilt("running", 0.1) otherwise.
        if (cc->velocity().magnitude() > 0.1f && (mobile || !hugging) && !sweeping)
        {
          ResetGuilt("running", 0.1f);
        }
      }
      else
      {
        playerSpeed = walkSpeed;
        if (sensitivityActive)
        {
          sensitivity = Mathf::Clamp((movement + lateralMovement).magnitude(), 0.0f, 1.0f);
        }
        else
        {
          sensitivity = 1.0f;
        }
      }
    }
    else
    {
      playerSpeed = walkSpeed;
      if (sensitivityActive)
      {
        sensitivity = Mathf::Clamp((lateralMovement + movement).magnitude(), 0.0f, 1.0f);
      }
      else
      {
        sensitivity = 1.0f;
      }
    }
    playerSpeed *= Time::deltaTime();
    moveDirection = bb::normalized(movement + lateralMovement) * playerSpeed * sensitivity;
    if (!(!jumpRope && !sweeping && !hugging))
    {
      if (sweeping && !bootsActive)
      {
        moveDirection = gottaSweep->velocity() * Time::deltaTime() + moveDirection * 0.3f;
      }
      else if (hugging && !bootsActive)
      {
        moveDirection = (firstPrize->velocity() * 1.2f * Time::deltaTime() + (Vec3(firstPrizeTransform->position.x, height, firstPrizeTransform->position.z) + Vec3((float)Mathf::RoundToInt(firstPrizeTransform->forward().x), 0.0f, (float)Mathf::RoundToInt(firstPrizeTransform->forward().z)) * 3.0f - transform->position)) * (float)principalBugFixer;
      }
      else if (jumpRope)
      {
        moveDirection = Vec3(0.0f, 0.0f, 0.0f);
      }
    }
    cc->Move(moveDirection);
  }

  void StaminaCheck() // private (C#)
  {
    // -debug cheat ("infinite stamina"): the tank never drains and the bar stays pinned to
    // 100 -- the effect the real drain would fight, without touching any authentic value. The
    // flag read is out-of-line (dbgStaminaPinned, body at the bottom of GameControllerScript.hpp)
    // because PlayerScript.hpp only forward-declares GameControllerScript.
    if (dbgStaminaPinned(gc))
    {
      stamina = maxStamina;
      staminaBar->value = 100.0f;
      return;
    }
    if (cc->velocity().magnitude() > 0.1f)
    {
      if ((ApplicationIsMobilePlatform() ? ReInputPlayer().GetButton("Run")   // iOS: Rewired "Run"
                                         : Singleton<InputManager>::Instance()->GetActionKey(InputAction::Run)) && stamina > 0.0f)
      {
        stamina -= staminaRate * Time::deltaTime();
      }
      if (stamina < 0.0f && stamina > -5.0f)
      {
        stamina = -5.0f;
      }
    }
    else if (stamina < maxStamina)
    {
      stamina += staminaRate * Time::deltaTime();
    }
    staminaBar->value = stamina / maxStamina * 100.0f;
  }

  virtual void OnTriggerEnter(Collider* other); // body: bottom of GameControllerScript.hpp (gc->debugMode)

  void OnTriggerStay(Collider* other) // private (C#)
  {
    if (other->transform->name() == "Gotta Sweep")
    {
      sweeping = true;
      sweepingFailsave = 1.0f;
    }
    else if (other->transform->name() == "1st Prize" && firstPrize->velocity().magnitude() > 5.0f)
    {
      hugging = true;
      sweepingFailsave = 1.0f;
    }
  }

  void OnTriggerExit(Collider* other) // private (C#)
  {
    if (other->transform->name() == "Office Trigger")
    {
      ResetGuilt("escape", door->lockTime);
    }
    else if (other->transform->name() == "Gotta Sweep")
    {
      sweeping = false;
    }
    else if (other->transform->name() == "1st Prize")
    {
      hugging = false;
    }
  }

  void ResetGuilt(const char* type, float amount)
  {
    if (amount >= guilt)
    {
      guilt = amount;
      guiltType = type;
    }
  }

  void GuiltCheck() // private (C#)
  {
    if (guilt > 0.0f)
    {
      guilt -= Time::deltaTime();
    }
  }

  void ActivateJumpRope()
  {
    jumpRopeScreen->SetActive(true);
    jumpRope = true;
    frozenPosition = transform->position;
  }

  void DeactivateJumpRope()
  {
    jumpRopeScreen->SetActive(false);
    jumpRope = false;
  }

  void ActivateBoots()
  {
    bootsActive = true;
    StartCoroutine(new BootTimer(this));
  }

  // ============ serialized fields (C# field-block order) ============

  GameControllerScript* gc = 0;
  BaldiScript* baldi = 0;
  DoorScript* door = 0;
  PlaytimeScript* playtime = 0;
  bool gameOver = false;
  bool jumpRope = false;
  bool sweeping = false;
  bool hugging = false;
  bool bootsActive = false;
  int principalBugFixer = 0;
  float sweepingFailsave = 0.0f;
  float fliparoo = 0.0f;
  float flipaturn = 0.0f;
  Quat playerRotation;                 // private (C#)
  Vec3 frozenPosition;                 // serialized
  bool sensitivityActive = false;      // private (C#)
  float sensitivity = 0.0f;            // private (C#)
  float mouseSensitivity = 0.0f;
  float walkSpeed = 0.0f;
  float runSpeed = 0.0f;
  float slowSpeed = 0.0f;              // unused in C# code, kept serialized
  float maxStamina = 0.0f;
  float staminaRate = 0.0f;
  float guilt = 0.0f;
  float initGuilt = 0.0f;              // unused in C# code, kept serialized
  Vec3 moveDirection;                  // private (C#)
  float playerSpeed = 0.0f;            // private (C#)
  float stamina = 0.0f;
  CharacterController* cc = 0;
  NavMeshAgent* gottaSweep = 0;
  NavMeshAgent* firstPrize = 0;
  Transform* firstPrizeTransform = 0;
  Slider* staminaBar = 0;
  float db = 0.0f;                     // unused in C# code, kept serialized
  std::string guiltType;
  GameObject* jumpRopeScreen = 0;
  float height = 0.0f;
  Material* blackSky = 0;
  Canvas* hud = 0;
};

// ---- coroutine state-machine bodies (touch PlayerScript members only) --------------------------

inline bool KeepTheHudOff::tick(float dt)
{
  if (player->gameOver)
  {
    player->hud->enabled = false;
    player->jumpRopeScreen->SetActive(false);
    return true; // Unity: yield return new WaitForEndOfFrame() -> one tick per frame
  }
  return false; // yield break
}

inline bool BootTimer::tick(float dt)
{
  // C# local time starts at 15; Unity's first iteration runs in the starting frame.
  time -= dt;
  if (time > 0.0f) return true;
  player->bootsActive = false;
  return false;
}

} // namespace bb
