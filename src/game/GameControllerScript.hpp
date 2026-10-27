#pragma once
#include "../eng.h"
#include "StubScripts.hpp"
#include "BaldiScript.hpp" // real translation (no GC dependency)  -  GC inline bodies read baldiScrpt members
#include "AILocationSelectorScript.hpp" // real: Teleport body reads AILocationSelector members
#include "CursorControllerScript.hpp"   // real: LockMouse/UnlockMouse bodies call LockCursor/UnlockCursor
#include "PlayerScript.hpp" // replaces its StubScripts entry; GC bodies use player members inline
#include "CameraScript.hpp" // real: Fliparoo body writes GetComponent<CameraScript>()->offset
#include "PlaytimeScript.hpp" // real: UseItem item-9 body calls playtimeScript->Disappoint()
#include "SwingingDoorScript.hpp" // real: UseItem item-2 GetComponent<...>::LockDoor needs complete
#include "FirstPrizeScript.hpp"  // real: UseItem item-9 firstPrizeScript->GoCrazy() needs complete
#include "DoorScript.hpp"        // real: UseItem items 3/8 GetComponent<DoorScript> flows
#include "TapePlayerScript.hpp"  // real: UseItem items 5/6 GetComponent<TapePlayerScript>::Play
#include "AlarmClockScript.hpp"  // real: UseItem item-7 GetComponent<AlarmClockScript>::baldi assign
#include "EntranceScript.hpp"   // mutual dep: carries fwd-decls only, so it is safe to pull here;
                                // its four out-of-line bodies are defined at the bottom of THIS file.
                                // MUST stay in this top-level block: an include placed inside
                                // `namespace bb` below nests a phantom bb::bb whenever another header
                                // (BullyScript) reaches GC.hpp before EntranceScript.hpp was seen.

// 1:1 translation of Assets\Scripts\Core\GameControllerScript.cs (Core batch; 656 lines).
// Replaces the placeholder that stood in while the first script batches compiled.
//
// Include architecture (mutual dependency with EntranceScript):
//   EntranceScript::Lower reads gc.finaleMode; GameControllerScript::ActivateSpoopMode /
//   ActivateFinaleMode call es->Lower/Raise. C# compiles per-file; C++ headers cannot be cyclic.
//   FOUR method bodies therefore live out-of-line at the bottom of THIS header (below the
//   EntranceScript include, where both classes are complete): GameControllerScript::
//   ActivateSpoopMode, ::ActivateFinaleMode, EntranceScript::Lower, EntranceScript::Raise.
//
// Translation notes (fidelity):
// - TMP_Text -> UiText (engine raster font, Phase 2): itemText, notebookCount.
// - uGUI surfaces: RawImage itemSlot[].texture; RectTransform anchoredPosition/localPosition for
//   itemSelect/boots (Unity's RectTransform derives Transform; minimal members until Phase 2 UI).
// - Rewired input -> our InputManager + InputAction enum (types.h); Input.GetAxis calls
//   ("Mouse ScrollWheel") belong to the engine input layer (Phase 2 / testbed keymap).
// - The C# bitwise `&` between side-effect-free bools became `&&` (behavior-identical).
// - Physics.Raycast(ray, out hit) -> Physics::Raycast(ray, &hit) (hit provider lands in Phase 3).
// - Coroutines -> Coroutine state machines (one tick == one `yield return null` frame).
// - Unity quirks preserved: audioDevice.PlayOneShot(aud_buzz) fires EVERY gameOver frame;
//   StartCoroutine starts enumerating synchronously in the caller's frame (engine contract:
//   first tick runs with the current frame's dt).
// - itemNames (C# private string[12], single consumer UpdateItemName) became a function-local
//   static table  -  C++11-safe (no static-member ODR dance) and behavior-identical.
// - Random.Range(12, 16) is the int overload -> 12..15 inclusive (Unity parity documented).
// - C# private Unity messages (Start/Update) are public overrides here  -  required by C++;
//   C# methods private in source are marked `// private (C#)`.

namespace bb {

struct GameControllerScript; // fwd (coroutines hold back-pointers)
struct EntranceScript;       // fwd (pointer fields below; full def included after the class)

// ---- Unity IEnumerator -> per-frame state machines -------------------------------------------

// private IEnumerator BootAnimation(): drops the boots UI banner from y=375 to -375 over time,
// idles `time` seconds, then raises it back. Bytes-for-bytes the same numeric flow.
struct BootAnim : Coroutine {
  GameControllerScript* gc = 0;
  float height = 375.0f;
  float time = 15.0f;
  int phase = 0; // entry statements, loop 1, loop 2, loop 3
  explicit BootAnim(GameControllerScript* gc_) : gc(gc_) {}
  bool tick(float dt); // defined below the class (needs complete members)
};

// private IEnumerator Teleporter(): blanks CharacterController + Collider, hops the player
// 12-15 times at a growing interval (0.2 * 1.1^n), pins player.height per flipped, re-enables.
struct TeleporterAnim : Coroutine {
  GameControllerScript* gc = 0;
  int teleports = 0, teleportCount = 0;
  float baseTime = 0.2f, currentTime = 0.2f, increaseFactor = 1.1f;
  int phase = 0; // entry statements run before the first yield (Unity)
  explicit TeleporterAnim(GameControllerScript* gc_) : gc(gc_) {}
  bool tick(float dt);
};

struct GameControllerScript : public Component {
  // ============ methods (C# listing order) ============

  virtual void Start() {
    cullingMask = playerCamera->cullingMask; // Changes cullingMask in the Camera
    audioDevice = GetComponent<AudioSource>(); // Get the Audio Source
    mode = PlayerPrefs::GetString("CurrentMode"); // Get the current mode
    if (mode == "endless") // If it is endless mode
    {
      baldiScrpt->endless = true; // Set Baldi use his slightly changed endless anger system
    }
    schoolMusic->Play(); // Play the school music
    LockMouse(); // Prevent the mouse from moving
    UpdateNotebookCount(); // Update the notebook count
    itemSelected = 0; // Set selection to item slot 0(the first item slot)
    gameOverDelay = 0.5f;
  }

  virtual void Update(float dt) {
    if (!learningActive)
    {
      // iOS: Rewired GetButtonDown("Pause"), with no gameOver test (ios-ref/classes/
      // GameControllerScript.ios.c Update); likewise Item / ItemSelect* / Item1-3 below.
      const bool mobile = ApplicationIsMobilePlatform();
      if (mobile ? ReInputPlayer().GetButtonDown("Pause")
                 : Singleton<InputManager>::Instance()->GetActionKeyDown(InputAction::PauseOrCancel) && !player->gameOver)
      {
        if (!gamePaused)
        {
          PauseGame();
        }
        else
        {
          UnpauseGame();
        }
      }

      if (Input::GetKeyDown(KeyCode::Y) && gamePaused)
      {
        ExitGame();
      }
      else if (Input::GetKeyDown(KeyCode::N) && gamePaused)
      {
        UnpauseGame();
      }

      if (!gamePaused && Time::timeScale != 1.0f)
      {
        Time::timeScale = 1.0f;
      }

      if ((mobile ? ReInputPlayer().GetButtonDown("Item") : Singleton<InputManager>::Instance()->GetActionKeyDown(InputAction::UseItem)) && Time::timeScale != 0.0f)
      {
        UseItem();
      }

      if (mobile)
      {
        if ((ReInputPlayer().GetAxis("ItemSelect") > 0.0f || ReInputPlayer().GetButtonDown("ItemSelectLeft")) && Time::timeScale != 0.0f)
        {
          DecreaseItemSelection();
        }
        else if ((ReInputPlayer().GetAxis("ItemSelect") < 0.0f || ReInputPlayer().GetButtonDown("ItemSelectRight")) && Time::timeScale != 0.0f)
        {
          IncreaseItemSelection();
        }
        if (Time::timeScale != 0.0f)
        {
          if (ReInputPlayer().GetButtonDown("Item1")) { itemSelected = 0; UpdateItemSelection(); }
          else if (ReInputPlayer().GetButtonDown("Item2")) { itemSelected = 1; UpdateItemSelection(); }
          else if (ReInputPlayer().GetButtonDown("Item3")) { itemSelected = 2; UpdateItemSelection(); }
        }
      }
      else if (Input::GetAxis("Mouse ScrollWheel") > 0.0f && Time::timeScale != 0.0f)
      {
        DecreaseItemSelection();
      }
      else if (Input::GetAxis("Mouse ScrollWheel") < 0.0f && Time::timeScale != 0.0f)
      {
        IncreaseItemSelection();
      }

      if (!mobile && Time::timeScale != 0.0f)
      {
        if (Singleton<InputManager>::Instance()->GetActionKey(InputAction::Slot0))
        {
          itemSelected = 0;
          UpdateItemSelection();
        }
        else if (Singleton<InputManager>::Instance()->GetActionKey(InputAction::Slot1))
        {
          itemSelected = 1;
          UpdateItemSelection();
        }
        else if (Singleton<InputManager>::Instance()->GetActionKey(InputAction::Slot2))
        {
          itemSelected = 2;
          UpdateItemSelection();
        }
      }
    }
    else
    {
      if (Time::timeScale != 0.0f)
      {
        Time::timeScale = 0.0f;
      }
    }

    if (player->stamina < 0.0f && !warning->activeSelf)
    {
      warning->SetActive(true); // Set the warning text to be visible
    }
    else if (player->stamina > 0.0f && warning->activeSelf)
    {
      warning->SetActive(false); // Set the warning text to be invisible
    }

    if (player->gameOver)
    {
      if (mode == "endless" && notebooks > PlayerPrefs::GetInt("HighBooks") && !highScoreText->activeSelf)
      {
        highScoreText->SetActive(true);
      }

      Time::timeScale = 0.0f;
      gameOverDelay -= Time::unscaledDeltaTime() * 0.5f;
      playerCamera->farClip = gameOverDelay * 400.0f; // Set camera farClip
      audioDevice->PlayOneShot(aud_buzz); // note: fires EVERY frame while gameOver (Unity behavior)

      if (gameOverDelay <= 0.0f)
      {
        if (mode == "endless")
        {
          if (notebooks > PlayerPrefs::GetInt("HighBooks"))
          {
            PlayerPrefs::SetInt("HighBooks", notebooks);
          }
          PlayerPrefs::SetInt("CurrentBooks", notebooks);
        }
        Time::timeScale = 1.0f;
        LoadScene(GameOverScene);
      }
    }

    if (finaleMode && !audioDevice->isPlaying() && exitsReached == 3)
    {
      audioDevice->clip = aud_MachineLoop;
      audioDevice->loop = true;
      audioDevice->Play();
    }
  }

  void UpdateNotebookCount() // private (C#)
  {
    if (mode == "story")
    {
      notebookCount->SetText(std::to_string(notebooks) + "/7 Notebooks");
    }
    else
    {
      notebookCount->SetText(std::to_string(notebooks) + " Notebooks");
    }

    if (notebooks == 7 && mode == "story")
    {
      ActivateFinaleMode();
    }
  }

  void CollectNotebook()
  {
    notebooks++;
    UpdateNotebookCount();
  }

  void LockMouse()
  {
    if (!learningActive)
    {
      cursorController->LockCursor(); // Prevent the cursor from moving
      mouseLocked = true;
      reticle->SetActive(true);
    }
  }

  void UnlockMouse()
  {
    cursorController->UnlockCursor(); // Allow the cursor to move
    mouseLocked = false;
    reticle->SetActive(false);
  }

  void PauseGame()
  {
    if (!learningActive)
    {
      {
        UnlockMouse();
      }
      Time::timeScale = 0.0f;
      gamePaused = true;
      pauseMenu->SetActive(true);
    }
  }

  void ExitGame()
  {
    Time::timeScale = 1.0f;
    LoadScene(ExitGameScene);
  }

  // iOS BUILD ONLY (absent from the PC decompile): the genuine iOS 1.4.3 binary's
  // GameControllerScript.OnApplicationPause is `if (!gamePaused) PauseGame();` -- it ignores
  // pauseStatus, so suspending AND resuming both leave the game paused (ios-ref/classes/
  // GameControllerScript.ios.c; gamePaused@400 per the recovered field offsets).
  void OnApplicationPause(bool pauseStatus) override
  {
    if (!gamePaused)
    {
      PauseGame();
    }
  }

  void UnpauseGame()
  {
    Time::timeScale = 1.0f;
    gamePaused = false;
    pauseMenu->SetActive(false);
    LockMouse();
  }

  void ActivateSpoopMode(); // out-of-line: needs EntranceScript complete (defined below)

  void ActivateFinaleMode(); // out-of-line: needs EntranceScript complete (defined below)

  void GetAngry(float value) // Make Baldi get angry
  {
    if (!spoopMode)
    {
      ActivateSpoopMode();
    }
    baldiScrpt->GetAngry(value);
  }

  void ActivateLearningGame()
  {
    // camera.cullingMask = 0; //Sets the cullingMask to nothing (commented in the C#)
    learningActive = true;
    UnlockMouse(); // Unlock the mouse
    tutorBaldi->Stop(); // Make tutor Baldi stop talking
    if (!spoopMode) // If the player hasn't gotten a question wrong
    {
      schoolMusic->Stop(); // Start playing the learn music
      learnMusic->Play();
    }
  }

  void DeactivateLearningGame(GameObject* subject)
  {
    playerCamera->cullingMask = cullingMask; // Sets the cullingMask to Everything
    learningActive = false;
    Destroy(subject);
    LockMouse(); // Prevent the mouse from moving
    if (player->stamina < 100.0f) // Reset Stamina
    {
      player->stamina = 100.0f;
    }
    if (!spoopMode) // If it isn't spoop mode, play the school music
    {
      schoolMusic->Play();
      learnMusic->Stop();
    }
    if (notebooks == 1 && !spoopMode) // If this is the players first notebook and they didn't get any questions wrong, reward them with a quarter
    {
      quarter->SetActive(true);
      tutorBaldi->PlayOneShot(aud_Prize);
    }
    else if (notebooks == 7 && mode == "story") // Plays the all 7 notebook sound
    {
      audioDevice->PlayOneShot(aud_AllNotebooks, 0.8f);
    }
  }

  void IncreaseItemSelection() // private (C#)
  {
    itemSelected++;
    if (itemSelected > 2)
    {
      itemSelected = 0;
    }
    itemSelect->anchoredPosition = Vec2((float)itemSelectOffset[itemSelected], (float)itemSelectPosition); // Moves the item selector background(the red rectangle)
    UpdateItemName();
  }

  void DecreaseItemSelection() // private (C#)
  {
    itemSelected--;
    if (itemSelected < 0)
    {
      itemSelected = 2;
    }
    itemSelect->anchoredPosition = Vec2((float)itemSelectOffset[itemSelected], (float)itemSelectPosition); // Moves the item selector background(the red rectangle)
    UpdateItemName();
  }

  void UpdateItemSelection() // private (C#)
  {
    itemSelect->anchoredPosition = Vec2((float)itemSelectOffset[itemSelected], (float)itemSelectPosition); // Moves the item selector background(the red rectangle)
    UpdateItemName();
  }

  void CollectItem(int item_ID)
  {
    if (item[0] == 0)
    {
      item[0] = item_ID; // Set the item slot to the Item_ID provided
      itemSlot[0]->texture = itemTextures[item_ID]; // Set the item slot's texture to a texture in a list of textures based on the Item_ID
    }
    else if (item[1] == 0)
    {
      item[1] = item_ID; // Set the item slot to the Item_ID provided
      itemSlot[1]->texture = itemTextures[item_ID]; // Set the item slot's texture to a texture in a list of textures based on the Item_ID
    }
    else if (item[2] == 0)
    {
      item[2] = item_ID; // Set the item slot to the Item_ID provided
      itemSlot[2]->texture = itemTextures[item_ID]; // Set the item slot's texture to a texture in a list of textures based on the Item_ID
    }
    else // This one overwrites the currently selected slot when your inventory is full
    {
      item[itemSelected] = item_ID;
      itemSlot[itemSelected]->texture = itemTextures[item_ID];
    }
    UpdateItemName();
  }

  void UseItem() // private (C#)
  {
    if (item[itemSelected] != 0)
    {
      if (item[itemSelected] == 1)
      {
        player->stamina = player->maxStamina * 2.0f;
        ResetItem();
        // player.ResetGuilt("food", 3f);  -  commented out in the C#
      }
      else if (item[itemSelected] == 2)
      {
        Ray ray = Camera::main()->ScreenPointToRay(Vec3((float)(Screen::width() / 2), (float)(Screen::height() / 2), 0.0f));
        RaycastHit raycastHit;
        if (Physics::Raycast(ray, &raycastHit) && (raycastHit.collider->tag() == "SwingingDoor" && dist(playerTransform->position, raycastHit.transform->position) <= 10.0f))
        {
          raycastHit.collider->gameObject->GetComponent<SwingingDoorScript>()->LockDoor(15.0f);
          ResetItem();
        }
      }
      else if (item[itemSelected] == 3)
      {
        Ray ray2 = Camera::main()->ScreenPointToRay(Vec3((float)(Screen::width() / 2), (float)(Screen::height() / 2), 0.0f));
        RaycastHit raycastHit2;
        if (Physics::Raycast(ray2, &raycastHit2) && (raycastHit2.collider->tag() == "Door" && dist(playerTransform->position, raycastHit2.transform->position) <= 10.0f))
        {
          DoorScript* component = raycastHit2.collider->gameObject->GetComponent<DoorScript>();
          if (component->DoorLocked())
          {
            component->UnlockDoor();
            component->OpenDoor();
            ResetItem();
          }
        }
      }
      else if (item[itemSelected] == 4)
      {
        Instantiate(bsodaSpray, playerTransform->position, cameraTransform->rotation);
        ResetItem();
        player->ResetGuilt("drink", 1.0f);
        audioDevice->PlayOneShot(aud_Soda);
      }
      else if (item[itemSelected] == 5)
      {
        Ray ray3 = Camera::main()->ScreenPointToRay(Vec3((float)(Screen::width() / 2), (float)(Screen::height() / 2), 0.0f));
        RaycastHit raycastHit3;
        if (Physics::Raycast(ray3, &raycastHit3))
        {
          if (raycastHit3.collider->gameObject->name == "BSODAMachine" && dist(playerTransform->position, raycastHit3.transform->position) <= 10.0f)
          {
            ResetItem();
            CollectItem(4);
          }
          else if (raycastHit3.collider->gameObject->name == "ZestyMachine" && dist(playerTransform->position, raycastHit3.transform->position) <= 10.0f)
          {
            ResetItem();
            CollectItem(1);
          }
          else if (raycastHit3.collider->gameObject->name == "PayPhone" && dist(playerTransform->position, raycastHit3.transform->position) <= 10.0f)
          {
            raycastHit3.collider->gameObject->GetComponent<TapePlayerScript>()->Play();
            ResetItem();
          }
        }
      }
      else if (item[itemSelected] == 6)
      {
        Ray ray4 = Camera::main()->ScreenPointToRay(Vec3((float)(Screen::width() / 2), (float)(Screen::height() / 2), 0.0f));
        RaycastHit raycastHit4;
        if (Physics::Raycast(ray4, &raycastHit4) && (raycastHit4.collider->gameObject->name == "TapePlayer" && dist(playerTransform->position, raycastHit4.transform->position) <= 10.0f))
        {
          raycastHit4.collider->gameObject->GetComponent<TapePlayerScript>()->Play();
          ResetItem();
        }
        // (the C# keeps a commented-out older "tape recordings" variant here)
      }
      else if (item[itemSelected] == 7)
      {
        GameObject* gameObject = Instantiate(alarmClock, playerTransform->position, cameraTransform->rotation);
        gameObject->GetComponent<AlarmClockScript>()->baldi = baldiScrpt;
        ResetItem();
      }
      else if (item[itemSelected] == 8)
      {
        Ray ray5 = Camera::main()->ScreenPointToRay(Vec3((float)(Screen::width() / 2), (float)(Screen::height() / 2), 0.0f));
        RaycastHit raycastHit5;
        if (Physics::Raycast(ray5, &raycastHit5) && (raycastHit5.collider->tag() == "Door" && dist(playerTransform->position, raycastHit5.transform->position) <= 10.0f))
        {
          raycastHit5.collider->gameObject->GetComponent<DoorScript>()->SilenceDoor();
          ResetItem();
          audioDevice->PlayOneShot(aud_Spray);
        }
      }
      else if (item[itemSelected] == 9)
      {
        Ray ray6 = Camera::main()->ScreenPointToRay(Vec3((float)(Screen::width() / 2), (float)(Screen::height() / 2), 0.0f));
        RaycastHit raycastHit6;
        if (player->jumpRope)
        {
          player->DeactivateJumpRope();
          playtimeScript->Disappoint();
          ResetItem();
        }
        else if (Physics::Raycast(ray6, &raycastHit6) && raycastHit6.collider->gameObject->name == "1st Prize")
        {
          firstPrizeScript->GoCrazy();
          ResetItem();
        }
      }
      else if (item[itemSelected] == 10)
      {
        player->ActivateBoots();
        StartCoroutine(new BootAnim(this));
        ResetItem();
      }
      else if (item[itemSelected] == 11)
      {
        StartCoroutine(new TeleporterAnim(this));
        ResetItem();
      }
    }
  }

  void Teleport() // private (C#)
  {
    AILocationSelector->GetNewTarget();
    player->transform->position = AILocationSelector->transform->position + Vec3(0.0f, 1.0f, 0.0f) * player->height; // Vector3.up * player.height
    audioDevice->PlayOneShot(aud_Teleport);
  }

  void ResetItem() // private (C#)
  {
    item[itemSelected] = 0;
    itemSlot[itemSelected]->texture = itemTextures[0];
    UpdateItemName();
  }

  void LoseItem(int id)
  {
    item[id] = 0;
    itemSlot[id]->texture = itemTextures[0];
    UpdateItemName();
  }

  void UpdateItemName() // private (C#)
  {
    // C# private string[12] field; sole consumer is this method -> function-local static table.
    static const char* const itemNames[12] = {
      "Nothing",
      "Energy flavored Zesty Bar",
      "Yellow Door Lock",
      "Principal's Keys",
      "BSODA",
      "Quarter",
      "Baldi Anti Hearing and Disorienting Tape",
      "Alarm Clock",
      "WD-NoSquee (Door Type)",
      "Safety Scissors",
      "Big Ol' Boots",
      "Teleportation Teleporter"
    };
    itemText->SetText(itemNames[item[itemSelected]]);
  }

  void ExitReached()
  {
    exitsReached++;
    if (exitsReached == 1)
    {
      setAmbientLight(Color::red()); // Make everything red
      // RenderSettings.fog = true;  -  commented out in the C#
      audioDevice->PlayOneShot(aud_Switch, 0.8f);
      audioDevice->clip = aud_MachineQuiet;
      audioDevice->loop = true;
      audioDevice->Play(); //start playing the weird sound
    }
    if (exitsReached == 2) //Play a sound
    {
      audioDevice->volume = 0.8f;
      audioDevice->clip = aud_MachineStart;
      audioDevice->loop = true;
      audioDevice->Play();
    }
    if (exitsReached == 3) //Play a even louder sound
    {
      audioDevice->clip = aud_MachineRev;
      audioDevice->loop = false;
      audioDevice->Play();
    }
  }

  void Fliparoo()
  {
    flipped = true;
    player->height = 6.0f;
    player->fliparoo = 180.0f;
    player->flipaturn = -1.0f;
    Camera::main()->GetComponent<CameraScript>()->offset = Vec3(0.0f, -1.0f, 0.0f);
  }

  // ============ serialized fields (C# field-block order; bindings reconcile in Phase 1) ============

  CursorControllerScript* cursorController = 0;
  PlayerScript* player = 0;
  Transform* playerTransform = 0;
  CharacterController* playerCharacter = 0;
  Collider* playerCollider = 0;
  AILocationSelectorScript* AILocationSelector = 0;
  Transform* cameraTransform = 0;
  Camera* playerCamera = 0;
  int cullingMask = 0;                     // private (C#)
  EntranceScript* entrance_0 = 0;
  EntranceScript* entrance_1 = 0;
  EntranceScript* entrance_2 = 0;
  EntranceScript* entrance_3 = 0;
  GameObject* baldiTutor = 0;
  GameObject* baldi = 0;
  BaldiScript* baldiScrpt = 0;
  AudioClip* aud_Prize = 0;
  AudioClip* aud_PrizeMobile = 0;          // unused in C# code, kept serialized
  AudioClip* aud_AllNotebooks = 0;
  AudioClip* aud_Teleport = 0;
  bool flipped = false;                    // private (C#)
  GameObject* principal = 0;
  GameObject* crafters = 0;
  GameObject* playtime = 0;
  PlaytimeScript* playtimeScript = 0;
  GameObject* gottaSweep = 0;
  GameObject* bully = 0;
  GameObject* firstPrize = 0;
  GameObject* TestEnemy = 0;               // stays serialized; its activation call is commented in the C#
  FirstPrizeScript* firstPrizeScript = 0;
  GameObject* quarter = 0;
  AudioSource* tutorBaldi = 0;
  RectTransform* boots = 0;
  std::string mode;
  int notebooks = 0;
  int failedNotebooks = 0;                 // unused in C# code, kept serialized
  bool spoopMode = false;
  bool finaleMode = false;
  bool debugMode = false;                  // unused in C# code, kept serialized
  bool mouseLocked = false;
  int exitsReached = 0;
  int itemSelected = 0;
  int item[3] = {0, 0, 0};
  RawImage* itemSlot[3] = {0, 0, 0};
  // (C# private string[] itemNames  -  moved into UpdateItemName as a local static table)
  UiText* itemText = 0;                    // TMP_Text -> UiText
  // 12, not the C# initializer's 10: the scene serialises 12 (0 = none ... 10 Big Ol' Boots,
  // 11 Teleportation Teleporter), and Unity's serialised array replaces the initializer. With 10,
  // CollectItem(10/11) read past the end of the array into the neighbouring fields.
  Texture* itemTextures[12] = {0};
  GameObject* bsodaSpray = 0;
  GameObject* alarmClock = 0;
  UiText* notebookCount = 0;               // TMP_Text -> UiText
  GameObject* pauseMenu = 0;
  std::string ExitGameScene;
  GameObject* highScoreText = 0;
  GameObject* warning = 0;
  GameObject* reticle = 0;
  RectTransform* itemSelect = 0;
  int itemSelectOffset[3] = {0, 0, 0};     // scene data supplies the offsets; recount at Phase 1 binding
  int itemSelectPosition = 0;
  bool gamePaused = false;                 // private (C#)
  bool learningActive = false;             // private (C#)
  float gameOverDelay = 0.0f;              // private (C#)
  std::string GameOverScene;
  AudioSource* audioDevice = 0;            // private (C#)
  AudioClip* aud_Soda = 0;
  AudioClip* aud_Spray = 0;
  AudioClip* aud_buzz = 0;
  AudioClip* aud_Hang = 0;
  AudioClip* aud_MachineQuiet = 0;
  AudioClip* aud_MachineStart = 0;
  AudioClip* aud_MachineRev = 0;
  AudioClip* aud_MachineLoop = 0;
  AudioClip* aud_Switch = 0;
  AudioSource* schoolMusic = 0;
  AudioSource* learnMusic = 0;
};

// EntranceScript is complete (included at the top of this file, outside namespace bb) and
// GameControllerScript is complete as of the closing brace above: everything below this line
// can see both classes.

// ---- out-of-line bodies (single definition site; breaks the include cycle) ---------------------

inline void EntranceScript::Lower()
{
  transform->position = transform->position - Vec3(0.0f, 10.0f, 0.0f);
  if (gc->finaleMode)
  {
    wall->material = map;
  }
}

inline void EntranceScript::Raise()
{
  transform->position = transform->position + Vec3(0.0f, 10.0f, 0.0f);
}

inline void GameControllerScript::ActivateSpoopMode()
{
  // TEMP BB_MATHDBG (remove with the debug gates): which spoop ref is null.
  if (getenv("BB_MATHDBG")) {
    printf("[math] spoop refs e0=%d e1=%d e2=%d e3=%d tutor=%d baldi=%d principal=%d crafters=%d "
           "playtime=%d sweep=%d bully=%d firstPrize=%d audioDev=%d learnMusic=%d schoolMusic=%d\n",
           (int)(entrance_0 != 0), (int)(entrance_1 != 0), (int)(entrance_2 != 0), (int)(entrance_3 != 0),
           (int)(baldiTutor != 0), (int)(baldi != 0), (int)(principal != 0), (int)(crafters != 0),
           (int)(playtime != 0), (int)(gottaSweep != 0), (int)(bully != 0), (int)(firstPrize != 0),
           (int)(audioDevice != 0), (int)(learnMusic != 0), (int)(schoolMusic != 0));
    fflush(stdout);
  }
  spoopMode = true; //Tells the game its time for spoop
  entrance_0->Lower(); //Lowers all the exits
  entrance_1->Lower();
  entrance_2->Lower();
  entrance_3->Lower();
  baldiTutor->SetActive(false); //Turns off Baldi(The one that you see at the start of the game)
  baldi->SetActive(true); //Turns on Baldi
  principal->SetActive(true); //Turns on Principal
  crafters->SetActive(true); //Turns on Crafters
  playtime->SetActive(true); //Turns on Playtime
  gottaSweep->SetActive(true); //Turns on Gotta Sweep
  bully->SetActive(true); //Turns on Bully
  firstPrize->SetActive(true); //Turns on First-Prize
  //TestEnemy.SetActive(true); //Turns on Test-Enemy (Bonus)  -  commented out in the C#
  audioDevice->PlayOneShot(aud_Hang); //Plays the hang sound
  learnMusic->Stop(); //Stop all the music
  schoolMusic->Stop();
}

inline void GameControllerScript::ActivateFinaleMode()
{
  finaleMode = true;
  entrance_0->Raise(); //Raise all the enterances(make them appear)
  entrance_1->Raise();
  entrance_2->Raise();
  entrance_3->Raise();
}

// ---- out-of-line bodies (PlayerScript side of the GC <-> PlayerScript mutual dependency) ------

inline void PlayerScript::Update(float dt)
{
  transform->position = Vec3(transform->position.x, height, transform->position.z);
  MouseMove();
  PlayerMove();
  StaminaCheck();
  GuiltCheck();
  if (cc->velocity().magnitude() > 0.0f)
  {
    gc->LockMouse();
  }
  if (jumpRope && (transform->position - frozenPosition).magnitude() >= 1.0f) // If the player moves, deactivate the jumprope minigame
  {
    DeactivateJumpRope();
  }
  if (sweepingFailsave > 0.0f)
  {
    sweepingFailsave -= Time::deltaTime();
  }
  else
  {
    sweeping = false;
    hugging = false;
  }
}

inline void PlayerScript::OnTriggerEnter(Collider* other)
{
  if (other->transform->name() == "Baldi" && !gc->debugMode)
  {
    gameOver = true;
    setSkybox(blackSky); //Sets the skybox black
    StartCoroutine(new KeepTheHudOff(this)); //Hides the Hud
  }
  else if (other->transform->name() == "Playtime" && !jumpRope && playtime->playCool <= 0.0f)
  {
    ActivateJumpRope();
  }
}

// ---- out-of-line bodies (SwingingDoorScript side of the GC <-> SwingingDoor dependency) --------

inline void SwingingDoorScript::Update(float dt)
{
  if (!requirementMet && gc->notebooks >= 2)
  {
    requirementMet = true;
    UnlockDoor();
  }
  if (openTime > 0.0f)
  {
    openTime -= 1.0f * Time::deltaTime();
  }
  if (lockTime > 0.0f)
  {
    lockTime -= Time::deltaTime();
  }
  else if (bDoorLocked && requirementMet)
  {
    UnlockDoor();
  }
  if (openTime <= 0.0f && bDoorOpen && !bDoorLocked)
  {
    bDoorOpen = false;
    inside->material = closed;
    outside->material = closedOutside;
  }
}

inline void SwingingDoorScript::OnTriggerEnter(Collider* other)
{
  if (!(gc->notebooks < 2 && other->tag() == "Player"))
  {
    if (!bDoorLocked)
    {
      myAudio->PlayOneShot(doorOpen, 1.0f);
      if (other->tag() == "Player" && baldi->isActiveAndEnabled())
      {
        baldi->Hear(transform->position, 1.0f);
      }
    }
  }
}

// ---- coroutine state-machine bodies -------------------------------------------------------------

inline bool BootAnim::tick(float dt)
{
  if (phase == 0) // IEnumerator entry (Unity runs it before the first yield)
  {
    gc->boots->gameObject->SetActive(true);
    phase = 1; // fall through: the first loop iteration runs in the same frame
  }
  if (phase == 1) // while (height > -375f): one iteration per frame
  {
    height -= 375.0f * dt;
    time -= dt;
    Vec3 position = gc->boots->localPosition;
    position.y = height;
    gc->boots->localPosition = position;
    if (height > -375.0f) return true;
    Vec3 position2 = gc->boots->localPosition; // trailing statements (same frame as the last pass)
    position2.y = -375.0f;
    gc->boots->localPosition = position2;
    gc->boots->gameObject->SetActive(false);
    phase = 2;
    return true;
  }
  if (phase == 2) // while (time > 0f): one decrement per frame
  {
    time -= dt;
    if (time > 0.0f) return true;
    gc->boots->gameObject->SetActive(true);
    phase = 3;
    return true;
  }
  // phase 3: while (height < 375f)
  height += 375.0f * dt;
  Vec3 position = gc->boots->localPosition;
  position.y = height;
  gc->boots->localPosition = position;
  if (height < 375.0f) return true;
  Vec3 position2 = gc->boots->localPosition;
  position2.y = 375.0f;
  gc->boots->localPosition = position2;
  gc->boots->gameObject->SetActive(false);
  return false; // yield break
}

inline bool TeleporterAnim::tick(float dt)
{
  if (phase == 0) // IEnumerator entry (Unity runs it before the first yield)
  {
    gc->playerCharacter->enabled = false;
    gc->playerCollider->enabled = false;
    teleports = Random::range(12, 16); // Unity int overload: exclusive max -> 12..15
    teleportCount = 0;
    baseTime = 0.2f;
    currentTime = baseTime;
    phase = 1; // fall through: the first loop body runs in the same frame
  }
  if (teleportCount < teleports)
  {
    currentTime -= dt;
    if (currentTime < 0.0f)
    {
      gc->Teleport();
      teleportCount++;
      baseTime *= increaseFactor;
      currentTime = baseTime;
    }
    if (gc->flipped)
    {
      gc->player->height = 6.0f;
    }
    else
    {
      gc->player->height = 4.0f;
    }
    return true;
  }
  gc->playerCharacter->enabled = true; // trailing statements (same frame as the final body pass)
  gc->playerCollider->enabled = true;
  return false;
}

} // namespace bb
