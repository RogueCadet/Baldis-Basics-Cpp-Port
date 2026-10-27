// game-actions.cpp  -  the seam between the scene loader and the translated game scripts.
//
// Unity's inspector-wired Button.onClick entries call METHODS ON SCRIPTS: StartGame on
// StartButton, ExitGame on ExitButtonScript, UnpauseGame on GameControllerScript. The loader
// (eng-scene.cpp) deliberately cannot see the game types -- it is engine layer -- so it hands the
// resolved Button + target Component + method NAME to this file, which can.
//
// Without this the menu navigates (SetActive is engine-only) but nothing ever STARTS the game:
// the Story/Endless buttons were inert.
//
// Only the methods the scenes actually wire are handled; anything else is reported by the caller
// rather than silently dropped. Adding one is three lines here, by design.
// Must precede every include: translated headers' env-gated debug tags call getenv (C4996 /W3).
#define _CRT_SECURE_NO_WARNINGS
#include "eng-internal.h"

#include "game/ControlsMenu.hpp"
#include "game/StartButton.hpp"
#include "game/ExitButtonScript.hpp"
#include "game/GameControllerScript.hpp"
#include "game/MathGameScript.hpp"   // You Can Think Pad: ButtonPress(int)/OKButton wiring
#include "game/MouseoverScript.hpp"
#include "game/TextUnderliner.hpp"
#include "game/UiSettings.hpp"
#include "game/SecretScript.hpp"
#include "game/PlayerScript.hpp"

namespace eng {
namespace {

// One adapter per wired method. They hold the already-resolved component, so Invoke is a direct
// call -- no name lookup at click time.
struct StartGameAction : public bb::UnityAction {
  bb::StartButton* self;
  explicit StartGameAction(bb::StartButton* s) : self(s) {}
  void Invoke() override { if (self) self->StartGame(); }
};
struct ExitGameAction : public bb::UnityAction {
  bb::ExitButtonScript* self;
  explicit ExitGameAction(bb::ExitButtonScript* s) : self(s) {}
  void Invoke() override { if (self) self->ExitGame(); }
};
struct UnpauseAction : public bb::UnityAction {
  bb::GameControllerScript* self;
  explicit UnpauseAction(bb::GameControllerScript* s) : self(s) {}
  void Invoke() override { if (self) self->UnpauseGame(); }
};

// MouseoverScript hover -> MainMenu underline swap (TextUnderliner)
struct UnderlineAction : public bb::UnityAction {
  bb::TextUnderliner* self;
  explicit UnderlineAction(bb::TextUnderliner* s) : self(s) {}
  void Invoke() override { if (self) self->Underline(); }
};
struct UnunderlineAction : public bb::UnityAction {
  bb::TextUnderliner* self;
  explicit UnunderlineAction(bb::TextUnderliner* s) : self(s) {}
  void Invoke() override { if (self) self->Ununderline(); }
};
// Options menu: Toggle.onValueChanged -> UiSettings.UpdateState (writes UiSize/UiHeight prefs)
struct UpdateStateAction : public bb::UnityAction {
  bb::UiSettings* self;
  explicit UpdateStateAction(bb::UiSettings* s) : self(s) {}
  void Invoke() override { if (self) self->UpdateState(); }
};
// MainMenu: the keyboard icon button loads the secret word scene
struct SecretLoadAction : public bb::UnityAction {
  bb::SecretScript* self;
  explicit SecretLoadAction(bb::SecretScript* s) : self(s) {}
  void Invoke() override { if (self) self->LoadScene(); }
};

// MathGame keypad: every number/minus/clear button calls ButtonPress(N) with a serialized
// m_IntArgument (0..9 digits, -1 minus, -2 clear) -- the pointer way to answer the pad
// questions without a keyboard.
struct ButtonPressAction : public bb::UnityAction {
  bb::MathGameScript* self;
  int value;
  ButtonPressAction(bb::MathGameScript* s, int v) : self(s), value(v) {}
  void Invoke() override { if (self) self->ButtonPress(value); }
};
// MathGame OK! button -> CheckAnswer (same public entry the Enter key reaches)
struct OKButtonAction : public bb::UnityAction {
  bb::MathGameScript* self;
  explicit OKButtonAction(bb::MathGameScript* s) : self(s) {}
  void Invoke() override { if (self) self->CheckAnswer(); }
};

// ControlsMenu (the controls page): the 35 persistent calls that used to go undispatched.
// StartRebindKeyboard/StartRebindMouse carry the action index as the serialized m_IntArgument.
struct ControlsMenuAction : public bb::UnityAction {
  bb::ControlsMenu* self;
  int which, arg;
  ControlsMenuAction(bb::ControlsMenu* s, int w, int a) : self(s), which(w), arg(a) {}
  void Invoke() override {
    if (!self) return;
    switch (which) {
      case 0: self->StartRebindKeyboard(arg); break;
      case 1: self->StartRebindMouse(arg); break;
      case 2: self->ShowResetConfirmation(); break;
      case 3: self->ResetKey(); break;
      case 4: self->ConfirmRebind(); break;
      case 5: self->ConfirmReset(); break;
      case 6: self->CancelRebind(); break;
      case 7: self->CancelReset(); break;
    }
  }
};

struct GcExitAction : public bb::UnityAction {
  bb::GameControllerScript* self;
  explicit GcExitAction(bb::GameControllerScript* s) : self(s) {}
  void Invoke() override { if (self) self->ExitGame(); }
};

// Maps (target component, method name) -> adapter, shared by the Button.onClick seam and the
// script-side serialized-events seam so the method table lives in exactly one place.
bb::UnityAction* actionFor(bb::Component* target, const std::string& method, int intArg) {
  if (method == "StartGame") {
    if (bb::StartButton* sb = dynamic_cast<bb::StartButton*>(target)) return new StartGameAction(sb);
  } else if (method == "ExitGame") {
    if (bb::ExitButtonScript* eb = dynamic_cast<bb::ExitButtonScript*>(target)) {
      return new ExitGameAction(eb);
    }
    // the pause menu's YES (BaldiNodButton) calls GameControllerScript.ExitGame, not the menu one
    if (bb::GameControllerScript* gcx = dynamic_cast<bb::GameControllerScript*>(target)) {
      return new GcExitAction(gcx);
    }
  } else if (method == "UnpauseGame") {
    if (bb::GameControllerScript* gc = dynamic_cast<bb::GameControllerScript*>(target)) {
      return new UnpauseAction(gc);
    }
  } else if (method == "Underline") {
    if (bb::TextUnderliner* tu = dynamic_cast<bb::TextUnderliner*>(target)) {
      return new UnderlineAction(tu);
    }
  } else if (method == "Ununderline") {
    if (bb::TextUnderliner* tu = dynamic_cast<bb::TextUnderliner*>(target)) {
      return new UnunderlineAction(tu);
    }
  } else if (method == "UpdateState") {
    if (bb::UiSettings* us = dynamic_cast<bb::UiSettings*>(target)) {
      return new UpdateStateAction(us);
    }
  } else if (method == "LoadScene") {
    if (bb::SecretScript* ss = dynamic_cast<bb::SecretScript*>(target)) {
      return new SecretLoadAction(ss);
    }
  } else if (method == "ButtonPress") {
    // Pad keypad buttons: the serialized m_IntArgument selects the typed value, handed in by
    // the loader through wireScriptCall's intArg.
    if (bb::MathGameScript* mg = dynamic_cast<bb::MathGameScript*>(target)) {
      return new ButtonPressAction(mg, intArg);
    }
  } else if (bb::ControlsMenu* cm = dynamic_cast<bb::ControlsMenu*>(target)) {
    static const char* const names[8] = { "StartRebindKeyboard", "StartRebindMouse", "ShowResetConfirmation",
                                          "ResetKey", "ConfirmRebind", "ConfirmReset", "CancelRebind", "CancelReset" };
    for (int k = 0; k < 8; k++) if (method == names[k]) return new ControlsMenuAction(cm, k, intArg);
  } else if (method == "OKButton") {
    if (bb::MathGameScript* mg = dynamic_cast<bb::MathGameScript*>(target)) {
      return new OKButtonAction(mg);
    }
  }
  return 0;
}

} // namespace

bool wireScriptCall(bb::Button* button, bb::Component* target, const std::string& method,
                    int intArg) {
  if (!button || !target) return false;
  bb::UnityAction* act = actionFor(target, method, intArg);
  if (!act) return false;
  button->onClick.AddListener(act);
  state().ownedActions.push_back(act);   // engine owns the adapter; engineShutdown frees it
  return true;
}

bool wirePersistentEvent(bb::Component* owner, const std::string& field, bb::Component* target,
                         const std::string& method) {
  if (!owner || !target) return false;
  bb::UnityAction* act = actionFor(target, method, 0);
  if (!act) return false;
  // Script-owned events (MouseoverScript...): resolve the UnityEvent member by field name.
  if (bb::MouseoverScript* mo = dynamic_cast<bb::MouseoverScript*>(owner)) {
    bb::UnityEvent* ev = field == "mouseOver" ? &mo->mouseOver
                       : field == "mouseLeave" ? &mo->mouseLeave
                       : (bb::UnityEvent*)0;
    if (ev) {
      ev->AddListener(act);
      state().ownedActions.push_back(act);   // engine owns the adapter; engineShutdown frees it
      return true;
    }
  }
  // Engine-owned uGUI event (Toggle.onValueChanged): Unity serializes the call list on the
  // component itself, which is exactly what the loader queues.
  if (bb::Toggle* tg = dynamic_cast<bb::Toggle*>(owner)) {
    if (field == "onValueChanged") {
      tg->onValueChanged.AddListener(act);
      state().ownedActions.push_back(act);
      return true;
    }
  }
  return false;
}

bool dispatchPointerEvent(bb::GameObject* go, bool enter) {
  if (!go || !go->activeInHierarchy()) return false;
  bool handled = false;
  for (size_t i = 0; i < go->components.size(); i++) {
    bb::Component* c = go->components[i];
    if (!c || !c->enabled) continue;
    bb::MouseoverScript* mo = dynamic_cast<bb::MouseoverScript*>(c);
    if (!mo) continue;
    if (enter) mo->OnPointerEnter(0);
    else mo->OnPointerExit(0);
    handled = true;
  }
  return handled;
}

} // namespace eng

// Shell --facing: PlayerScript.MouseMove writes its private playerRotation over transform.rotation
// every Update (PlayerScript.cs:50), so setting only the transform was undone on the next frame.
// Set both, the way the game itself holds the facing. Returns false when there is no player.
namespace eng {
bool setPlayerFacing(float yawDeg) {
  bb::PlayerScript* ps = bb::FindObjectOfType<bb::PlayerScript>();
  if (!ps || !ps->transform) return false;
  ps->transform->rotation.setEulerAngles(bb::Vec3(0.0f, yawDeg, 0.0f));
  ps->playerRotation = ps->transform->rotation;
  return true;
}
}

// Shell --catch N: Baldi catches the player at frame N, through the genuine path -- spoop mode on,
// Baldi placed 3 units in front of the player and facing him (as he is when his trigger reaches
// the player), then PlayerScript.OnTriggerEnter(Baldi's collider). Lets the jumpscare (camera on
// Baldi, HUD off, far clip closing to black, BAL_Screech) be captured without playing to it.
namespace eng {
bool debugBaldiCatch() {
  bb::PlayerScript* ps = bb::FindObjectOfType<bb::PlayerScript>();
  bb::GameControllerScript* gc = bb::FindObjectOfType<bb::GameControllerScript>();
  if (!ps || !gc || !ps->transform) return false;
  if (!gc->spoopMode) gc->ActivateSpoopMode();
  bb::GameObject* baldi = bb::GameObject::Find("Baldi");
  if (!baldi || !baldi->transform) return false;
  const bb::Vec3 fwd = ps->transform->forward();
  bb::NavMeshAgent* agent = baldi->GetComponent<bb::NavMeshAgent>();
  const bb::Vec3 at = ps->transform->position + bb::Vec3(fwd.x, 0.0f, fwd.z) * 3.0f;
  if (agent) agent->Warp(at); else baldi->transform->position = at;
  const float yaw = atan2f(-fwd.x, -fwd.z);
  baldi->transform->rotation = bb::Quat(0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f));
  bb::Collider* col = baldi->GetComponent<bb::CapsuleCollider>();
  if (!col) return false;
  ps->OnTriggerEnter(col);
  return ps->gameOver;
}
}

// Shell --spoop: wake every NPC (GameControllerScript.ActivateSpoopMode, what the first notebook's
// wrong answer does) so the full gameplay load can be profiled.
namespace eng {
bool debugSpoopMode() {
  bb::GameControllerScript* gc = bb::FindObjectOfType<bb::GameControllerScript>();
  if (!gc) return false;
  if (!gc->spoopMode) gc->ActivateSpoopMode();
  return true;
}
}
