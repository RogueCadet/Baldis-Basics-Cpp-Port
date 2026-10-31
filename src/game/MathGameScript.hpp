#pragma once
#include "../eng.h"
#include "GameControllerScript.hpp" // gc -> complete (ActivateLearningGame, notebooks, mode, ...)

// 1:1 translation of Assets\Scripts\Core\UI\MathGameScript.cs (407 lines; the you-can-think-pad).
// Pulls StubScripts transitively (BaldiScript stub supplies Hear/GetAngry/GetTempAngry until the
// Baldi AI batch).
//
// Translation notes (fidelity):
// - TMP_InputField -> InputField; TMP_Text -> UiText; uGUI RawImage results, Button firstButton.
// - The cheat codes are kept verbatim: "31718" -> TestRoom (LoadSceneAsync), "53045009" -> Fliparoo.
//   Note the C# does NOT return after either cheat branch  -  the problem<=3 block still runs.
// - Impossible-mode float concatenation went through .NET float.ToString(); those numerals route
//   through csharpToString() (engine impl; format verified against the real game in the testbed).
// - question "strings" replicated exactly, including the authentic missing ')' in the impossible
//   problems ("+(" num2 "X" num3 "=")  -  artifact of the original, kept.
// - System.GC.Collect() before ExitGame() is a .NET no-op for us; kept as a comment.
// - hintText / endlessHintText (C# private string[]) have a single consumer (NewProblem) and are
//   never Unity-serialized (private), so they are function-local static tables  -  behavior-identical.
// - CheatText coroutine runs forever (per-frame text refresh); engine phase 2 frees it at the
//   scene unload. WaitForEndOfFrame -> one tick per frame (same convention as PlayerScript).
// - C# private methods/fields marked `// private (C#)`; bitwise `&` on side-effect-free bools -> `&&`.
namespace bb {

struct MathGameScript; // fwd (defined below; the CheatText coroutine holds a back-pointer)

// private IEnumerator CheatText(string text): infinite per-frame text refresh.
struct CheatText : Coroutine {
  MathGameScript* mg = 0;   // fwd-declared below; body defined after the class
  std::string text;
  CheatText(MathGameScript* mg_, const std::string& text_) : mg(mg_), text(text_) {}
  bool tick(float dt);
};

// private string[] hintText / endlessHintText (C# fields; non-serialized, sole consumer is
// NewProblem)  -  namespace scope with internal linkage, placed before the class so the member
// body's unqualified name resolves.
const char* const hintText[2] = {
  "I GET ANGRIER FOR EVERY PROBLEM YOU GET WRONG",
  "I HEAR EVERY DOOR YOU OPEN"
};
const char* const endlessHintText[2] = {
  "That's more like it...",
  "Keep up the good work or see me after class..."
};

struct MathGameScript : public Component {
  // ============ methods (C# listing order) ============

  virtual void Start() {
    gc->ActivateLearningGame();
    if (gc->notebooks == 1)
    {
      QueueAudio(bal_intro);
      QueueAudio(bal_howto);
    }
    NewProblem();
    if (gc->spoopMode)
    {
      baldiFeedTransform->position = Vec3(-1000.0f, -1000.0f, 0.0f);
    }
  }

  virtual void Update(float dt) {
    if (!baldiAudio->isPlaying())
    {
      if (audioInQueue > 0 && !gc->spoopMode)
      {
        PlayQueue();
      }
      baldiFeed->SetBool("talking", false);
    }
    else
    {
      baldiFeed->SetBool("talking", true);
    }
    if ((Input::GetKeyDown("return") || Input::GetKeyDown("enter")) && questionInProgress)
    {
      questionInProgress = false;
      CheckAnswer();
    }
    // -debug cheat ("auto-correct math questions"): one tick after a problem appears, the input is
    // filled with the right answer and OK'd for you. The impossible corrupted question (problem 3)
    // is auto-answered WITH "0" -- a deliberately WRONG submission, exactly as if the player had
    // typed junk and pressed enter (baldi gets angrier; spoop mode kicks in) -- instead of being
    // skipped silently. Pressing / mid-notebook still opens the menu instead. (blank = problem
    // submitted >3, i.e. the notebook is done -- the auto-answer never touches it.)
    if (gc->dbgAutoAnswer && questionInProgress && problem <= 3 && !autoAnswerDone)
    {
      autoAnswerDone = true;
      questionInProgress = false;
      playerAnswer->SetText(impossibleMode ? "0" : csharpToString(solution));
      CheckAnswer();
    }
    if (problem > 3)
    {
      endDelay -= 1.0f * Time::unscaledDeltaTime();
      if (endDelay <= 0.0f)
      {
        // System.GC.Collect();  -  .NET only, no-op for the C++ port (kept for parity notes)
        ExitGame();
      }
    }
  }

  void NewProblem() // private (C#)
  {
    playerAnswer->SetText("");
    problem++;
    autoAnswerDone = false; // -debug cheat: each new problem gets exactly one auto-submit
    playerAnswer->ActivateInputField();
    // TEMP BB_MATHDBG (remove after the thinkpad is verified interactive): branch/inputs.
    if (getenv("BB_MATHDBG")) {
      printf("[math] NewProblem problem=%d mode='%s' notebooks=%d timeScale=%.2f gc=%d\n",
             problem, gc ? gc->mode.c_str() : "{null gc}", gc ? gc->notebooks : -1,
             Time::timeScale, (int)(gc != 0));
    }
    if (problem <= 3)
    {
      QueueAudio(bal_problems[problem - 1]);
      if ((gc->mode == "story" && (problem <= 2 || gc->notebooks <= 1)) || (gc->mode == "endless" && (problem <= 2 || gc->notebooks != 2)))
      {
        num1 = (float)Mathf::RoundToInt(Random::range(0.0f, 9.0f));
        num2 = (float)Mathf::RoundToInt(Random::range(0.0f, 9.0f));
        sign = Mathf::RoundToInt(Random::range(0.0f, 1.0f));
        QueueAudio(bal_numbers[Mathf::RoundToInt(num1)]);
        if (sign == 0)
        {
          solution = num1 + num2;
          questionText->SetText("SOLVE MATH Q" + std::to_string(problem) + ": \n \n" + csharpToString(num1) + "+" + csharpToString(num2) + "=");
          QueueAudio(bal_plus);
        }
        else if (sign == 1)
        {
          solution = num1 - num2;
          questionText->SetText("SOLVE MATH Q" + std::to_string(problem) + ": \n \n" + csharpToString(num1) + "-" + csharpToString(num2) + "=");
          QueueAudio(bal_minus);
        }
        QueueAudio(bal_numbers[Mathf::RoundToInt(num2)]);
        QueueAudio(bal_equals);
      }
      else
      {
        impossibleMode = true;
        num1 = Random::range(1.0f, 9999.0f);
        num2 = Random::range(1.0f, 9999.0f);
        num3 = Random::range(1.0f, 9999.0f);
        sign = Mathf::RoundToInt((float)Random::range(0, 1));
        QueueAudio(bal_screech);
        // TEMP BB_MATHDBG: bisect impossible branch.
        if (getenv("BB_MATHDBG")) { printf("[math] impossible branch sign=%d\n", (int)sign); fflush(stdout); }
        if (sign == 0)
        {
          questionText->SetText("SOLVE MATH Q" + std::to_string(problem) + ": \n" + csharpToString(num1) + "+(" + csharpToString(num2) + "X" + csharpToString(num3) + "=");
          QueueAudio(bal_plus);
          QueueAudio(bal_screech);
          QueueAudio(bal_times);
          QueueAudio(bal_screech);
        }
        else if (sign == 1)
        {
          questionText->SetText("SOLVE MATH Q" + std::to_string(problem) + ": \n (" + csharpToString(num1) + "/" + csharpToString(num2) + ")+" + csharpToString(num3) + "=");
          QueueAudio(bal_divided);
          QueueAudio(bal_screech);
          QueueAudio(bal_plus);
          QueueAudio(bal_screech);
        }
        num1 = Random::range(1.0f, 9999.0f);
        num2 = Random::range(1.0f, 9999.0f);
        num3 = Random::range(1.0f, 9999.0f);
        sign = Mathf::RoundToInt((float)Random::range(0, 1));
        if (sign == 0)
        {
          questionText2->SetText("SOLVE MATH Q" + std::to_string(problem) + ": \n" + csharpToString(num1) + "+(" + csharpToString(num2) + "X" + csharpToString(num3) + "=");
        }
        else if (sign == 1)
        {
          questionText2->SetText("SOLVE MATH Q" + std::to_string(problem) + ": \n (" + csharpToString(num1) + "/" + csharpToString(num2) + ")+" + csharpToString(num3) + "=");
        }
        num1 = Random::range(1.0f, 9999.0f);
        num2 = Random::range(1.0f, 9999.0f);
        num3 = Random::range(1.0f, 9999.0f);
        sign = Mathf::RoundToInt((float)Random::range(0, 1));
        if (sign == 0)
        {
          questionText3->SetText("SOLVE MATH Q" + std::to_string(problem) + ": \n" + csharpToString(num1) + "+(" + csharpToString(num2) + "X" + csharpToString(num3) + "=");
        }
        else if (sign == 1)
        {
          questionText3->SetText("SOLVE MATH Q" + std::to_string(problem) + ": \n (" + csharpToString(num1) + "/" + csharpToString(num2) + ")+" + csharpToString(num3) + "=");
        }
        QueueAudio(bal_equals);
      }
      questionInProgress = true;
    }
    else
    {
      endDelay = 5.0f;
      if (!gc->spoopMode)
      {
        questionText->SetText("WOW! YOU EXIST!");
      }
      else if (gc->mode == "endless" && problemsWrong <= 0)
      {
        int num = Mathf::RoundToInt(Random::range(0.0f, 1.0f));
        questionText->SetText(endlessHintText[num]);
      }
      else if (gc->mode == "story" && problemsWrong >= 3)
      {
        questionText->SetText("I HEAR MATH THAT BAD");
        questionText2->SetText("");
        questionText3->SetText("");
        if (baldiScript->isActiveAndEnabled()) baldiScript->Hear(playerPosition, 7.0f);
        gc->failedNotebooks++;
      }
      else
      {
        int num2 = Mathf::RoundToInt(Random::range(0.0f, 1.0f));
        questionText->SetText(hintText[num2]);
        questionText2->SetText("");
        questionText3->SetText("");
      }
    }
  }

  void OKButton()
  {
    CheckAnswer();
  }

  void CheckAnswer()
  {
    // TEMP BB_MATHDBG (remove with the debug gates): pinpoint the OK!-dispatch crash.
    if (getenv("BB_MATHDBG")) {
      printf("[math] CheckAnswer answer='%s' problem=%d solution=%.1f impossible=%d results0=%d "
             "results1=%d results2=%d baldiFeed=%d baldiScript=%d baldiAudio=%d gc=%d\n",
             playerAnswer->text.c_str(), problem, solution, (int)impossibleMode,
             (int)(results[0] != 0), (int)(results[1] != 0), (int)(results[2] != 0),
             (int)(baldiFeed != 0), (int)(baldiScript != 0), (int)(baldiAudio != 0),
             (int)(gc != 0));
      fflush(stdout);
    }
    if (playerAnswer->text == "31718")
    {
      StartCoroutine(new CheatText(this, "THIS IS WHERE IT ALL BEGAN"));
      LoadSceneAsync("TestRoom");
    }
    else if (playerAnswer->text == "53045009")
    {
      StartCoroutine(new CheatText(this, "USE THESE TO STICK TO THE CEILING!"));
      gc->Fliparoo();
    }
    if (problem <= 3)
    {
      if (playerAnswer->text == csharpToString(solution) && !impossibleMode)
      {
        results[problem - 1]->texture = correct;
        baldiAudio->Stop();
        ClearAudioQueue();
        int num = Mathf::RoundToInt(Random::range(0.0f, 4.0f));
        QueueAudio(bal_praises[num]);
        NewProblem();
      }
      else
      {
        problemsWrong++;
        // TEMP BB_MATHDBG: bisect the crash between the calls below.
        if (getenv("BB_MATHDBG")) { printf("[math] else-branch: writing results\n"); fflush(stdout); }
        results[problem - 1]->texture = incorrect;
        if (!gc->spoopMode)
        {
          if (getenv("BB_MATHDBG")) { printf("[math] calling SetTrigger('angry')\n"); fflush(stdout); }
          baldiFeed->SetTrigger("angry");
          if (getenv("BB_MATHDBG")) { printf("[math] calling ActivateSpoopMode\n"); fflush(stdout); }
          gc->ActivateSpoopMode();
          if (getenv("BB_MATHDBG")) { printf("[math] ActivateSpoopMode returned\n"); fflush(stdout); }
        }
        if (gc->mode == "story")
        {
          if (problem == 3)
          {
            baldiScript->GetAngry(1.0f);
          }
          else
          {
            baldiScript->GetTempAngry(0.25f);
          }
        }
        else
        {
          // TEMP BB_MATHDBG: bisect tail.
          if (getenv("BB_MATHDBG")) { printf("[math] else-chain (mode='%s') GetAngry\n", gc->mode.c_str()); fflush(stdout); }
          baldiScript->GetAngry(1.0f);
        }
        ClearAudioQueue();
        // TEMP BB_MATHDBG: before Stop.
        if (getenv("BB_MATHDBG")) { printf("[math] calling baldiAudio->Stop\n"); fflush(stdout); }
        baldiAudio->Stop();
        // TEMP BB_MATHDBG: after Stop.
        if (getenv("BB_MATHDBG")) { printf("[math] calling NewProblem\n"); fflush(stdout); }
        NewProblem();
        if (getenv("BB_MATHDBG")) { printf("[math] CheckAnswer tail done\n"); fflush(stdout); }
      }
    }
  }

  void QueueAudio(AudioClip* sound) // private (C#)
  {
    audioQueue[audioInQueue] = sound;
    audioInQueue++;
  }

  void PlayQueue() // private (C#)
  {
    baldiAudio->PlayOneShot(audioQueue[0]);
    UnqueueAudio();
  }

  void UnqueueAudio() // private (C#)
  {
    for (int i = 1; i < audioInQueue; i++)
    {
      audioQueue[i - 1] = audioQueue[i];
    }
    audioInQueue--;
  }

  void ClearAudioQueue() // private (C#)
  {
    audioInQueue = 0;
  }

  void ExitGame() // private (C#)
  {
    if (problemsWrong <= 0 && gc->mode == "endless")
    {
      baldiScript->GetAngry(-1.0f);
    }
    gc->DeactivateLearningGame(gameObject);
  }

  void ButtonPress(int value)
  {
    if (value >= 0 && value <= 9)
    {
      playerAnswer->SetText(playerAnswer->text + std::to_string(value));
    }
    else if (value == -1)
    {
      playerAnswer->SetText(playerAnswer->text + "-");
    }
    else
    {
      playerAnswer->SetText("");
    }
  }

  // ============ serialized fields (C# field-block order) ============
  GameControllerScript* gc = 0;
  BaldiScript* baldiScript = 0;
  Vec3 playerPosition;                 // serialized
  GameObject* mathGame = 0;
  RawImage* results[3] = {0, 0, 0};
  Texture* correct = 0;
  Texture* incorrect = 0;
  InputField* playerAnswer = 0;        // TMP_InputField -> InputField
  UiText* questionText = 0;            // TMP_Text -> UiText
  UiText* questionText2 = 0;
  UiText* questionText3 = 0;
  Animator* baldiFeed = 0;
  Transform* baldiFeedTransform = 0;
  AudioClip* bal_plus = 0;
  AudioClip* bal_minus = 0;
  AudioClip* bal_times = 0;
  AudioClip* bal_divided = 0;
  AudioClip* bal_equals = 0;
  AudioClip* bal_howto = 0;
  AudioClip* bal_intro = 0;
  AudioClip* bal_screech = 0;
  AudioClip* bal_numbers[10] = {0};
  AudioClip* bal_praises[5] = {0};
  AudioClip* bal_problems[3] = {0};
  Button* firstButton = 0;             // serialized, unused in C# code
  float endDelay = 0.0f;               // private (C#)
  int problem = 0;                     // private (C#)
  int audioInQueue = 0;                // private (C#)
  float num1 = 0.0f;                   // private (C#)
  float num2 = 0.0f;                   // private (C#)
  float num3 = 0.0f;                   // private (C#)
  int sign = 0;                        // private (C#)
  float solution = 0.0f;               // private (C#)
  // (hintText / endlessHintText: private, non-serialized, sole consumer NewProblem -> namespace
  //  scope tables just above the class)
  bool questionInProgress = false;     // private (C#)
  bool autoAnswerDone = false;         // -debug cheat: one-shot per problem (reset in NewProblem)
  bool impossibleMode = false;         // private (C#)
  int problemsWrong = 0;               // private (C#)
  AudioClip* audioQueue[20] = {0};     // private (C#)
  AudioSource* baldiAudio = 0;
};

// ---- coroutine state-machine body (needs MathGameScript complete) -------------------------------

// private IEnumerator CheatText(string text):
//   for (;;) { questionText.text = text; questionText2.text = string.Empty;
//              questionText3.text = string.Empty; yield return new WaitForEndOfFrame(); }
// An unconditional infinite loop with one yield per iteration -> one tick per frame, never
// returning false. The engine frees the machine on scene unload (both cheats load a new scene or
// flip the world; neither path ever stops the refresh).
inline bool CheatText::tick(float dt)
{
  mg->questionText->SetText(text);
  mg->questionText2->SetText(std::string());
  mg->questionText3->SetText(std::string());
  return true;                         // yield return new WaitForEndOfFrame() -- runs forever
}

} // namespace bb
