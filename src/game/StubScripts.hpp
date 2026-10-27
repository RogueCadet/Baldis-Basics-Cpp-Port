#pragma once
#include "../eng.h"
#include "Singleton.hpp"

// InputManager  -  top-level include discipline (lesson #8): headers must include other headers
// OUTSIDE `namespace bb`, or their contents land in bb::bb and everything cascades.
#include "InputManager.hpp"

// STUBS ONLY  -  every script referenced by the Core translations but not yet translated.
// Each entry lists its pristine decompile path + the exact surface already-translated scripts
// touch. Pointer-only dependencies need nothing (fwd decl would do); method/field usages need
// complete types, hence these no-op stubs. EVERY stub is replaced by its real translation when
// its batch lands.
namespace bb {

struct BaldiScript; // fwd: pointer-member refs in stubs below (real translation: BaldiScript.hpp)

// (BaldiScript stub removed  -  real translation lives in src\game\BaldiScript.hpp; it has no
// GameControllerScript dependency, so no include-cycle work was needed for it)

// Assets\Scripts\PlayerScript.cs (Player batch, ~265 lines)  -  refs: stamina, maxStamina, gameOver,
// jumpRope, height, fliparoo, flipaturn, ResetGuilt, DeactivateJumpRope, ActivateBoots
// (PlayerScript stub removed  -  real translation lives in src\game\PlayerScript.hpp)

// Assets\Scripts\CursorControllerScript.cs  -  refs: LockCursor, UnlockCursor
// (CursorControllerScript stub removed -> real translation: src\game\CursorControllerScript.hpp)

// Assets\Scripts\AILocationSelectorScript.cs  -  refs: GetNewTarget
// (AILocationSelectorScript stub removed -> real translation: src\game\AILocationSelectorScript.hpp,
// which also retired AmbienceScript.cs on the way through)

// Assets\Scripts\SwingingDoorScript.cs  -  refs: LockDoor(float)
// (SwingingDoorScript stub removed -> real translation: src\game\SwingingDoorScript.hpp; its
// gc-reading bodies live out-of-line at the bottom of GameControllerScript.hpp)

// Assets\Scripts\DoorScript.cs  -  refs: DoorLocked, UnlockDoor, OpenDoor, SilenceDoor
// (DoorScript stub removed -> real translation: src\game\DoorScript.hpp; DoorLocked became a
// method, per the C# get-only property -> GetComponent users call it with parens)

// Assets\Scripts\TapePlayerScript.cs  -  refs: Play()
// (TapePlayerScript stub removed -> real translation: src\game\TapePlayerScript.hpp)

// Assets\Scripts\AlarmClockScript.cs  -  refs: baldi field
// (AlarmClockScript stub removed -> real translation: src\game\AlarmClockScript.hpp)

// Assets\Scripts\PlaytimeScript.cs  -  refs: Disappoint(), playCool
// (PlaytimeScript stub removed -> real translation: src\game\PlaytimeScript.hpp)

// Assets\Scripts\FirstPrizeScript.cs  -  refs: GoCrazy()
// (FirstPrizeScript stub removed -> real translation: src\game\FirstPrizeScript.hpp)

// Assets\Scripts\CameraScript.cs  -  refs: offset member (used by Fliparoo)
// (CameraScript stub removed -> real translation: src\game\CameraScript.hpp)

// Assets\Scripts\Core\UI\Settings\ControlMapper\InputManager.cs  -  REAL translation is included
// at the top of this file (src\game\InputManager.hpp): InputAction enum + per-action
// PlayerPrefs-backed bindings + simulated keys; singleton-arranged via Singleton<InputManager>.
// Device feeds are Phase 2's: SDL keyboard/mouse on the Windows testbed (eng::inject*), native
// touch on iOS.

} // namespace bb
