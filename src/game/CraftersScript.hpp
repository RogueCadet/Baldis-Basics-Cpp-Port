#pragma once
#include "../eng.h"
#include "GameControllerScript.hpp"     // gc.notebooks read (one-way: GC never references
                                        // CraftersScript  -  no cycle)

// 1:1 translation of Assets\Scripts\NPCFunctions\CraftersScript.cs (115 lines; Arts and Crafters
//  -  appears only in the endgame (7+ notebooks), chases on sight and warps both the player and
// Baldi to new locations on contact).
//
// Translation notes (fidelity):
// - FixedUpdate raycast: origin+direction overload, mask 769, PositiveInfinity, trigger-ignore;
//   the C# mixed `&` chain -> `&&` (side-effect-free operands; visibilityThink: Renderer::
//   isVisible added to eng.h).
// - On contact: player CharacterController disabled during the warp (player ends at y=4),
//   Baldi via NavMeshAgent::Warp, then the script's own gameObject deactivates.
// - `Random-free` speed growth: agent.speed += 60 * dt while angry.
// - `[SerializeField] private NavMeshAgent agent` stays private (serialized nonetheless).
// - GiveLocation(...) is called from other NPC scripts (flee = force-show for 3 s).
// - C# private members marked `// private (C#)`.

namespace bb {

struct CraftersScript : public Component {
  virtual void Start() {
    agent = GetComponent<NavMeshAgent>(); // Defines the nav mesh agent
    audioDevice = GetComponent<AudioSource>(); //Gets the audio source
    sprite->SetActive(false); // Set arts and crafters sprite to be invisible
  }

  virtual void Update(float dt) {
    if (forceShowTime > 0.0f)
    {
      forceShowTime -= Time::deltaTime();
    }
    if (gettingAngry) //If arts is getting agry
    {
      anger += Time::deltaTime(); // Increase anger
      if (anger >= 1.0f && !angry) //If anger is greater then 1 and arts isn't angry
      {
        angry = true; // Get angry
        audioDevice->PlayOneShot(aud_Intro); // Do the woooosoh sound
        spriteImage->sprite = angrySprite; // Switch to the angry sprite
      }
    }
    else if (anger > 0.0f) // If anger is greater then 0, decrease.
    {
      anger -= Time::deltaTime();
    }
    if (!angry) // If not angry
    {
      if (((transform->position - agent->destination()).magnitude() <= 20.0f && (transform->position - player->position).magnitude() >= 60.0f) || forceShowTime > 0.0f) //If close to the player and force showtime is less then 0
      {
        sprite->SetActive(true); // Become visible
      }
      else
      {
        sprite->SetActive(false); // Become invisible
      }
    }
    else
    {
      agent->speed = agent->speed + 60.0f * Time::deltaTime(); // Increase the speed
      TargetPlayer(); // Target the player
      if (!audioDevice->isPlaying()) //If the sound is not already playing
      {
        audioDevice->PlayOneShot(aud_Loop); //Play the full wooooosh sound
      }
    }
  }

  virtual void FixedUpdate() {
    if (gc->notebooks >= 7) // If the player has more then 7 notebooks
    {
      Vec3 direction = player->position - transform->position;
      RaycastHit raycastHit;
      if (Physics::Raycast(transform->position + Vec3(0.0f, 1.0f, 0.0f) * 2.0f, direction, &raycastHit, std::numeric_limits<float>::infinity(), 769, QueryTriggerInteraction::Ignore) && raycastHit.transform->tag() == "Player" && craftersRenderer->isVisible() && sprite->activeSelf) // If Arts is Visible, and active and sees the player
      {
        gettingAngry = true; // Start to get angry
      }
      else
      {
        gettingAngry = false; // Stop getting angry
      }
    }
  }

  void GiveLocation(const Vec3& location, bool flee)
  {
    if (!angry && agent->isActiveAndEnabled())
    {
      agent->SetDestination(location);
      if (flee)
      {
        forceShowTime = 3.0f; // Make arts appear in 3 seconds
      }
    }
  }

  void TargetPlayer() // private (C#)
  {
    agent->SetDestination(player->position); // Set destination to the player
  }

  virtual void OnTriggerEnter(Collider* other) { // private (C#)
    if (other->tag() == "Player" && angry) // If arts is angry and is touching the player
    {
      cc->enabled = false;
      player->position = Vec3(playerTeleportPoint->position.x, 4.0f, playerTeleportPoint->position.z); // Teleport the player to Player_WarpPoint
      baldiAgent->Warp(baldiTeleportPoint->position); // Teleport Baldi to Baldi_WarpPoint
      cc->enabled = true;
      gameObject->SetActive(false); // Despawn Arts And Crafters
    }
  }

  // ============ serialized fields (C# field-block order) ============
  bool db = false;
  bool angry = false;
  bool gettingAngry = false;
  float anger = 0.0f;
  float forceShowTime = 0.0f;          // private (C#)
  Transform* player = 0;
  CharacterController* cc = 0;
  Transform* playerCamera = 0;
  Transform* baldiTeleportPoint = 0;
  Transform* playerTeleportPoint = 0;
  NavMeshAgent* baldiAgent = 0;
  GameObject* sprite = 0;
  GameControllerScript* gc = 0;
  NavMeshAgent* agent = 0;             // private (C#)  -  [SerializeField]
  Renderer* craftersRenderer = 0;
  SpriteRenderer* spriteImage = 0;
  Sprite* angrySprite = 0;
  AudioSource* audioDevice = 0;        // private (C#)
  AudioClip* aud_Intro = 0;
  AudioClip* aud_Loop = 0;
};

} // namespace bb
