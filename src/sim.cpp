// sim.cpp -- headless validation harness for the engine + bake loader.
//
// Run from the PROJECT ROOT (data/scene-*.bake.json and data/manifest.json are CWD-relative):
//   build\sim.exe
// Prints one terse ASCII "PASS <name>" / "FAIL <name>: <detail>" line per check and exits
// nonzero if any check failed, so the build gate can treat it as a pass/fail oracle.
//
// This is the first end-to-end exercise of the School scene: pass 1-4 instantiation, world
// transform compose, script field binding (bindings.cpp), and 300 frames of the fixed-step loop.
// Checks are deliberately cheap and deterministic -- no gameplay driving yet (that needs the
// input surface and deep prefab cloning; see PLAN.md backlog).

// Must precede every include: MSVC flags getenv (the trace switch) as C4996 under /W3, and the
// suppression only applies if it is defined before the CRT headers are first pulled in.
#define _CRT_SECURE_NO_WARNINGS

#include "eng-internal.h"

#include "game/GameControllerScript.hpp"  // pulls PlayerScript/BaldiScript/CameraScript et al
#include "game/PickupAnimationScript.hpp"
#include "game/NotebookScript.hpp"
#include "game/StubScripts.hpp"           // InputManager (the DDoL singleton the swap checks pin)
// prefab-clone assertions name the components BSODA_Spray / MathGame author:
#include "game/BsodaSparyScript.hpp"
#include "game/Billboard.hpp"
#include "game/MathGameScript.hpp"
#include "game/PrincipalScript.hpp"
#include "game/TapePlayerScript.hpp"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <set>
#include <typeinfo>
#include <algorithm>

#if defined(_WIN32)
#include <direct.h>
#define SIM_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define SIM_MKDIR(p) mkdir((p), 0755)
#endif

namespace {

int gFails = 0;

// The DDoL input singleton, captured at the first swap so the cross-scene identity check can
// compare against it after all six scene loads (see testDdolStillAlive in main()).
bb::InputManager* g_ddolCapture = 0;

void pass(const char* name) { printf("PASS %s\n", name); }

void fail(const char* name, const std::string& detail) {
  printf("FAIL %s: %s\n", name, detail.c_str());
  gFails++;
}

void check(const char* name, bool ok, const std::string& detail = std::string()) {
  if (ok) pass(name); else fail(name, detail.empty() ? std::string("condition false") : detail);
}

// snprintf (not sprintf): MSVC deprecates the unbounded form under /W3 as C4996.
std::string f2s(float v) { char b[64]; snprintf(b, sizeof(b), "%.6g", v); return std::string(b); }
std::string i2s(long long v) { char b[32]; snprintf(b, sizeof(b), "%lld", v); return std::string(b); }

// ---- test 1: Quat euler round-trip -----------------------------------------------------------
// quat-euler.cpp carries the verified Unity conversions; this guards the pair against drift.
// Round-tripping EULERS is not well-posed (many triples alias to one orientation), so the
// invariant tested is the one that actually matters to the port: re-applying the recovered
// angles must reproduce the same ROTATION, i.e. identical action on basis vectors.
void testEulerRoundTrip() {
  // deterministic sample set (degrees), incl. axis-aligned and mixed cases
  const float samples[][3] = {
    {   0.0f,   0.0f,   0.0f }, {  90.0f,   0.0f,   0.0f }, {   0.0f,  90.0f,   0.0f },
    {   0.0f,   0.0f,  90.0f }, {  30.0f,  45.0f,  60.0f }, { -17.5f, 212.0f,  -98.25f },
    { 179.0f, 359.0f,   1.0f }, {  45.0f, 180.0f, 270.0f }, { -60.0f, -30.0f, -120.0f },
  };
  const int n = (int)(sizeof(samples) / sizeof(samples[0]));
  const bb::Vec3 basis[3] = { bb::Vec3(1,0,0), bb::Vec3(0,1,0), bb::Vec3(0,0,1) };
  float worst = 0.0f;
  int worstIdx = -1;
  for (int i = 0; i < n; i++) {
    bb::Vec3 deg(samples[i][0], samples[i][1], samples[i][2]);
    bb::Quat q = bb::Quat::euler(deg);
    bb::Quat q2 = bb::Quat::euler(q.eulerAngles());   // recovered angles re-applied
    for (int b = 0; b < 3; b++) {
      bb::Vec3 d = q.rotate(basis[b]) - q2.rotate(basis[b]);
      float e = d.magnitude();
      if (e > worst) { worst = e; worstIdx = i; }
    }
  }
  if (worst <= 1e-3f) pass("euler-roundtrip");
  else fail("euler-roundtrip", "max basis error " + f2s(worst) + " at sample " + i2s(worstIdx));
}

// ---- test 1b: Quat eulerAngles DECOMPOSITION (Unity's branch selection) ----------------------
// The round-trip above proves ROTATION preservation; this test pins the REPRESENTATION Unity
// returns (values asserted are Unity's observed read-backs - see quat-euler.cpp header). The
// first case is load-bearing: (0,1,0,0) is Secret's authored camera rotation, and
// PlayerScript::MouseMove re-applies (eulerAngles.x, eulerAngles.y, fliparoo) every frame - a
// read-back with pitch != 0 folds 180 deg of yaw into X and flips the camera upside down (the
// session 7r/7s root cause). The Rx(180) -> (0,180,180) case is Unity's documented-by-behaviour
// re-expression of a 180 deg X rotation. Gimbal case pins the fold. All tolerances 1e-3 deg,
// which is far above float path noise (< 1e-4 deg) and far below any visible-angle error.
void testEulerUnityDecomposition() {
  struct Case { const char* label; bb::Vec3 eq; bb::Vec3 want; };
  const Case cases[] = {
    { "Ry(180) -> (0,180,0)  [Secret camera]", bb::Vec3(0, 180, 0),    bb::Vec3(0, 180, 0) },
    { "Rx(180) -> (0,180,180) [Unity quirk]",  bb::Vec3(180, 0, 0),    bb::Vec3(0, 180, 180) },
    { "Rz(180) -> (0,0,180)",                  bb::Vec3(0, 0, 180),    bb::Vec3(0, 0, 180) },
    { "Rx(90)  -> (90,0,0)   [pitch-clamp]",   bb::Vec3(90, 0, 0),     bb::Vec3(90, 0, 0) },
    { "Rz(-90) -> (0,0,270)  [MakePositive]",  bb::Vec3(0, 0, -90),    bb::Vec3(0, 0, 270) },
    { "Ry(-10) -> (0,350,0)  [MakePositive]",  bb::Vec3(0, -10, 0),    bb::Vec3(0, 350, 0) },
    { "Ry(105) -> (0,105,0)  [yaw passthrough]", bb::Vec3(0, 105, 0),  bb::Vec3(0, 105, 0) },
    { "Ry(90)  -> (0,90,0)   [gimbal fold]",   bb::Vec3(0, 90, 0),     bb::Vec3(0, 90, 0) },
    { "Rx(30)xRy(90) -> (30,90,0) [gimbal fold keeps combination]", bb::Vec3(30, 90, 0), bb::Vec3(30, 90, 0) },
    { "Rx(120)xRy(30) -> (300,150,180) [alt tuple, pitch clamp]",   bb::Vec3(120, 30, 0), bb::Vec3(300, 150, 180) },
  };
  const int n = (int)(sizeof(cases) / sizeof(cases[0]));
  const float kTol = 1e-3f;
  for (int i = 0; i < n; i++) {
    bb::Quat q = bb::Quat::euler(cases[i].eq);
    bb::Vec3 got = q.eulerAngles();
    const char* label = cases[i].label;
    if (fabsf(got.x - cases[i].want.x) <= kTol &&
        fabsf(got.y - cases[i].want.y) <= kTol &&
        fabsf(got.z - cases[i].want.z) <= kTol) {
      pass((std::string("euler-decomp-") + label).c_str());
    } else {
      fail((std::string("euler-decomp-") + label).c_str(),
           "read-back (" + f2s(got.x) + ", " + f2s(got.y) + ", " + f2s(got.z) + ") deg, want (" +
           f2s(cases[i].want.x) + ", " + f2s(cases[i].want.y) + ", " + f2s(cases[i].want.z) + ")");
    }
  }
  // And the MouseMove chain itself: the exact operation PlayerScript::MouseMove applies to
  // Secret's start rotation must be an exact identity (no flip) on the first frame.
  bb::Quat pr = bb::Quat::euler(bb::Vec3(0.0f, 180.0f, 0.0f));   // Secret authored playerRotation
  pr.setEulerAngles(bb::Vec3(pr.eulerAngles().x, pr.eulerAngles().y, 0.0f)); // fliparoo == 0
  const float upErr = (pr.rotate(bb::Vec3(0, 1, 0)) - bb::Vec3(0, 1, 0)).magnitude();
  const float fwdErr = (pr.rotate(bb::Vec3(0, 0, 1)) - bb::Vec3(0, 0, -1)).magnitude();
  if (upErr <= 1e-4f && fwdErr <= 1e-4f) pass("euler-mousemove-identity");
  else fail("euler-mousemove-identity", "up error " + f2s(upErr) + ", forward error " + f2s(fwdErr));
}

// ---- test 2: RNG determinism -----------------------------------------------------------------
// Parity runs depend on this: same seed -> same stream, different seed -> different stream.
void testRandomDeterminism() {
  const int kN = 16;
  float a[kN], b[kN], c[kN];
  eng::setRandomSeed(12345);
  for (int i = 0; i < kN; i++) a[i] = bb::Random::range(0.0f, 1.0f);
  eng::setRandomSeed(12345);
  for (int i = 0; i < kN; i++) b[i] = bb::Random::range(0.0f, 1.0f);
  eng::setRandomSeed(54321);
  for (int i = 0; i < kN; i++) c[i] = bb::Random::range(0.0f, 1.0f);

  int mismatch = -1;
  for (int i = 0; i < kN; i++) if (a[i] != b[i]) { mismatch = i; break; }
  if (mismatch < 0) pass("random-reseed-repeats");
  else fail("random-reseed-repeats", "slot " + i2s(mismatch) + " " + f2s(a[mismatch]) +
                                     " != " + f2s(b[mismatch]));

  bool diverged = false;
  for (int i = 0; i < kN; i++) if (a[i] != c[i]) { diverged = true; break; }
  check("random-seeds-diverge", diverged, "seed 54321 produced the seed-12345 stream");

  // a constant stream would satisfy both checks above; require actual variation too
  bool varies = false;
  for (int i = 1; i < kN; i++) if (a[i] != a[0]) { varies = true; break; }
  check("random-stream-varies", varies, "all 16 samples identical");

  // int overload is exclusive on the upper bound (Unity parity)
  eng::setRandomSeed(777);
  bool inRange = true;
  for (int i = 0; i < 256; i++) {
    int v = bb::Random::range(3, 7);
    if (v < 3 || v > 6) { inRange = false; break; }
  }
  check("random-int-range", inRange, "Random::range(3,7) left 3..6");
}

// ---- test 3: PlayerPrefs round-trip + INI write -----------------------------------------------
// PlayerPrefs is file-backed; every setter calls prefsSave(), so the INI must exist afterwards.
void testPlayerPrefs(const char* prefsPath) {
  remove(prefsPath);                      // start from a known-absent file
  bb::PlayerPrefs::SetInt("simProbeInt", 4242);
  bb::PlayerPrefs::SetFloat("simProbeFloat", 0.5f);
  bb::PlayerPrefs::SetString("simProbeStr", "hello");

  int gotI = bb::PlayerPrefs::GetInt("simProbeInt", -1);
  check("prefs-int-roundtrip", gotI == 4242, "got " + i2s(gotI));
  float gotF = bb::PlayerPrefs::GetFloat("simProbeFloat", -1.0f);
  check("prefs-float-roundtrip", fabsf(gotF - 0.5f) < 1e-6f, "got " + f2s(gotF));
  std::string gotS = bb::PlayerPrefs::GetString("simProbeStr", "");
  check("prefs-string-roundtrip", gotS == "hello", "got '" + gotS + "'");
  check("prefs-missing-key-default", bb::PlayerPrefs::GetInt("simNoSuchKey", 99) == 99,
        "absent key did not return the supplied default");
  check("prefs-haskey", bb::PlayerPrefs::HasKey("simProbeInt") &&
                        !bb::PlayerPrefs::HasKey("simNoSuchKey"), "HasKey disagrees with the store");

  // (fopenSafe is file-static inside eng-core.cpp; jsonReadFile is the public read path.)
  bool ok = false;
  std::string ini = eng::jsonReadFile(prefsPath, ok);
  if (ok && ini.find("simProbeInt") != std::string::npos) pass("prefs-ini-written");
  else if (!ok) fail("prefs-ini-written", std::string("no readable file at ") + prefsPath);
  else fail("prefs-ini-written", std::string("INI at ") + prefsPath + " lacks the probe key");
}

// ---- School scene structural assertions -------------------------------------------------------
// The bake is the source of truth (School: 5,207 objects / 16,237 components), so a load that
// silently dropped most of the scene is caught by the count floor rather than by a crash later.
bb::GameObject* makeProbeObject(const char* name, const bb::Vec3& p);   // defined below
void testSchoolStructure() {
  eng::State& g = eng::state();
  size_t goCount = g.gos.size();
  check("school-go-count", goCount > 5000, "only " + i2s((long long)goCount) + " GameObjects");

  // Every transform must carry a finite, non-degenerate scale: a NaN or all-zero scale means the
  // world compose ran on uninitialised locals, and compose multiplies parent chains so the error
  // propagates to the whole subtree.
  // NOT tested: positivity. Unity permits negative scale (mirroring), and School uses it -- 81
  // transforms are legitimately negative on an axis (__AIPath_Walk_Trans is scaled -1 on x).
  int badScale = 0;
  std::string firstBad;
  for (size_t i = 0; i < g.transforms.size(); i++) {
    bb::Transform* t = g.transforms[i];
    if (dynamic_cast<bb::RectTransform*>(t) != 0) continue; // UI rects legally carry authored zero
                                                            // scale (hidden legacy buttons; the
                                                            // corruption detector is for 3D nodes)
    const bb::Vec3& s = t->scale;
    bool finite = (s.x == s.x) && (s.y == s.y) && (s.z == s.z);
    bool degenerate = (s.x == 0.0f && s.y == 0.0f && s.z == 0.0f);
    if (!finite || degenerate) {
      if (!badScale) {
        firstBad = t->name() + " scale (" + f2s(s.x) + "," + f2s(s.y) + "," + f2s(s.z) + ")";
      }
      badScale++;
    }
  }
  check("school-transform-scales", badScale == 0,
        i2s(badScale) + " transforms with NaN/all-zero scale (first: " + firstBad + ")");

  // Component back-pointers: pass 2 must wire gameObject/transform on every attached component,
  // because translated script bodies dereference both without a null guard (Unity guarantees them).
  int orphanComps = 0;
  for (size_t i = 0; i < g.gos.size(); i++) {
    bb::GameObject* go = g.gos[i];
    for (size_t j = 0; j < go->components.size(); j++) {
      if (go->components[j]->gameObject != go) orphanComps++;
    }
  }
  check("school-comp-backpointers", orphanComps == 0,
        i2s(orphanComps) + " components whose gameObject does not point at their owner");

  // ---- MeshCollider surface census --------------------------------------------------------------
  // School's 2618 MeshColliders all reference the built-in Quad (bake fileID 10210; 23 flagged
  // isTrigger -- these are the wall-trigger panels). bindBuiltin maps that built-in ref to an
  // analytic quad surface (eng.h); a regression there silently degrades every wall raycast to a
  // skip, so pin the census exactly. If a re-bake legitimately changes the counts, update here --
  // the FAIL line prints what actually loaded.
  int quad = 0, plane = 0, none = 0, meshTrig = 0;
  const bb::MeshCollider* firstNone = 0;
  for (size_t i = 0; i < g.meshColliders.size(); i++) {
    const bb::MeshCollider* m = g.meshColliders[i];
    if (m->surface == bb::MeshCollider::SurfaceQuad) quad++;
    else if (m->surface == bb::MeshCollider::SurfacePlane) plane++;
    else none++;
    if (m->surface == bb::MeshCollider::SurfaceNone && !firstNone) firstNone = m;
    if (m->isTrigger) meshTrig++;
  }
  std::string meshDetail = "quad=" + i2s(quad) + " plane=" + i2s(plane) +
                           " unrecognised=" + i2s(none) + " triggers=" + i2s(meshTrig);
  if (firstNone) meshDetail += std::string(" (first unrecognised: ") +
                               (firstNone->gameObject ? firstNone->gameObject->name : "?") + ")";
  check("school-mesh-quad-census", g.meshColliders.size() == 2618 && quad == 2618 &&
        plane == 0 && none == 0 && meshTrig == 23, meshDetail);
}

// ---- script binding assertions ----------------------------------------------------------------
// These are the fields the first gameplay frames dereference; a binder/loader fidelity gap shows
// up here as a null rather than as a crash 300 frames in.
// FindObjectOfType skips INACTIVE objects (Unity parity, eng-impl.h). School starts Baldi's
// GameObject with active:false -- he is switched on when the round begins -- so reaching his
// script at load time needs a sweep that ignores the active flag.
template <class T> T* findAnyIncludingInactive() {
  eng::State& g = eng::state();
  for (size_t i = 0; i < g.gos.size(); i++) {
    bb::GameObject* go = g.gos[i];
    for (size_t j = 0; j < go->components.size(); j++) {
      if (T* t = dynamic_cast<T*>(go->components[j])) return t;
    }
  }
  return 0;
}

bb::PlayerScript* testScriptBinding() {
  bb::GameControllerScript* gc = bb::FindObjectOfType<bb::GameControllerScript>();
  check("school-gamecontroller-present", gc != 0, "FindObjectOfType<GameControllerScript> null");

  if (gc) {
    // ActivateSpoopMode() -- the first notebook turns the game from a tutorial into a chase -- does
    // nothing but SetActive on eight GameObject references plus four entrance Lower() calls. If ANY
    // of those failed to bind, the chase silently never starts and the game is unplayable in a way
    // no render check can see. The bake carries all eight, so a null here is a BINDING fault.
    const char* spoopNames[8] = { "baldi", "baldiTutor", "principal", "crafters",
                                  "playtime", "gottaSweep", "bully", "firstPrize" };
    bb::GameObject* spoopRefs[8] = { gc->baldi, gc->baldiTutor, gc->principal, gc->crafters,
                                     gc->playtime, gc->gottaSweep, gc->bully, gc->firstPrize };
    int bound = 0;
    std::string missing;
    for (int i = 0; i < 8; i++) {
      if (spoopRefs[i]) bound++;
      else { if (!missing.empty()) missing += ", "; missing += spoopNames[i]; }
    }
    check("school-spoop-refs-bound", bound == 8,
          i2s(bound) + "/8 ActivateSpoopMode GameObject references bound" +
          (missing.empty() ? std::string() : (" -- null: " + missing)) +
          "; a null one means the first notebook starts no chase at all");
    // The authored pre-chase state: the tutorial Baldi is ON, the chasing Baldi is OFF.
    check("school-tutor-active-baldi-not",
          gc->baldiTutor != 0 && gc->baldi != 0 &&
          gc->baldiTutor->activeSelf && !gc->baldi->activeSelf,
          "the scene must start with baldiTutor ACTIVE and baldi INACTIVE (ActivateSpoopMode swaps "
          "them); anything else means the pre-chase state is wrong");
  }

  bb::PlayerScript* player = bb::FindObjectOfType<bb::PlayerScript>();
  check("school-player-present", player != 0, "FindObjectOfType<PlayerScript> null");
  if (player) {
    check("school-player-cc-bound", player->cc != 0,
          "PlayerScript::cc (CharacterController) stayed null after binding");
  }

  // REAL-LEVEL WANDER (user report: "teleported around and getting stuck"). The probe corner
  // proves one corner; this walks the ACTUAL player capsule through the ACTUAL School colliders,
  // 3000 steps of 0.12 with a seeded random heading every 60 steps, so it slides
  // along walls, into corners and through doorways. A TELEPORT is any single Move that displaces
  // the capsule further than it was asked to go (plus 0.05 for legitimate depenetration); a
  // collide-and-slide can only ever SHORTEN a move. Position is restored afterwards.
  if (player && player->cc && player->cc->transform) {
    bb::CharacterController* wcc = player->cc;
    const bb::Vec3 home = wcc->transform->position;
    int teleports = 0, frozen = 0, shortened = 0;
    float worst = 0.0f;
    int worstStep = -1;
    float heading = 0.0f;
    unsigned int lcg = 0x1234567u;                       // own LCG: must not perturb eng's RNG
    for (int st = 0; st < 3000; st++) {
      if (st % 60 == 0) {                                // a new random heading every 7.2 units
        lcg = lcg * 1664525u + 1013904223u;
        heading = (float)(lcg >> 8) * (6.2831853f / 16777216.0f);
      }
      const bb::Vec3 mv(sinf(heading) * 0.12f, 0.0f, cosf(heading) * 0.12f);
      const bb::Vec3 before = wcc->transform->position;
      wcc->Move(mv);
      const bb::Vec3 after = wcc->transform->position;
      const bb::Vec3 dd(after.x - before.x, 0.0f, after.z - before.z);
      const float moved = bb::length(dd);
      if (moved > 0.12f + 0.05f) {
        teleports++;
        if (moved > worst) { worst = moved; worstStep = st; }
      }
      if (moved < 0.001f) frozen++;
      if (moved < 0.119f) shortened++;
    }
    printf("WANDER teleports=%d worst=%.3f@%d frozenSteps=%d shortened=%d home=(%.1f,%.1f,%.1f) end=(%.1f,%.1f,%.1f)\n", teleports, worst, worstStep, frozen, shortened, home.x, home.y, home.z, wcc->transform->position.x, wcc->transform->position.y, wcc->transform->position.z);
    check("school-wander-no-teleport", teleports == 0,
          f2s((float)teleports) + " of 3000 steps displaced the player further than asked (worst " +
          f2s(worst) + " for a 0.12 move, step " + f2s((float)worstStep) + ")");
    // STUCK: a step that does not move at all. Walking head-on into a wall legitimately freezes
    // (measured: 22 of 3000 with the fix); the pre-fix slab logic froze 2572 of 3000 (86%).
    check("school-wander-not-stuck", frozen < 150,
          f2s((float)frozen) + " of 3000 steps did not move at all -- the capsule is wedging");
    wcc->transform->position = home;
  }

  // HIERARCHY AT RUNTIME (user: "the characters' image stays where it started while their
  // invisible self moves"; "the notebook does not disappear after it is used"). In School.unity
  // every character's SpriteRenderer is a CHILD of the object that moves, and a notebook's art is
  // its child _nbSprite. Move the real roots the way the game does and the children must follow.
  {
    struct H {
      static bb::GameObject* find(const char* n) {
        std::vector<bb::GameObject*>& all = eng::liveGameObjects();
        for (size_t i = 0; i < all.size(); i++) if (all[i]->name == n) return all[i];
        return 0;
      }
      static bb::Transform* child(bb::GameObject* go, const char* n) {
        std::vector<bb::GameObject*>& all = eng::liveGameObjects();
        for (size_t i = 0; i < all.size(); i++)
          if (all[i]->name == n && all[i]->transform && all[i]->transform->parent == go->transform) return all[i]->transform;
        return 0;
      }
    };
    bb::GameObject* baldi = H::find("Baldi");
    bb::Transform* bs = baldi ? H::child(baldi, "BaldiSprite") : 0;
    bb::GameObject* nb = H::find("GeographyNotebook");
    bb::Transform* ns = nb ? H::child(nb, "_nbSprite") : 0;
    if (baldi && bs && nb && ns) {
      const bb::Vec3 bHome = baldi->transform->position, nHome = nb->transform->position;
      const bb::Vec3 bOff = bs->position - bHome, nOff = ns->position - nHome;
      baldi->transform->position = bHome + bb::Vec3(6.0f, 0.0f, -3.0f);        // an agent step
      nb->transform->position = bb::Vec3(nHome.x, -20.0f, nHome.z);            // NotebookScript: used
      eng::runFullFrame(1.0f / 50.0f);
      const bb::Vec3 bNow = bs->position - baldi->transform->position;
      const bb::Vec3 nNow = ns->position - nb->transform->position;
      check("hier-character-sprite-follows", bb::length(bNow - bOff) < 1e-3f,
            "BaldiSprite offset from Baldi was (" + f2s(bOff.x) + "," + f2s(bOff.z) + "), after the move (" +
            f2s(bNow.x) + "," + f2s(bNow.z) + ") -- the sprite stayed behind");
      // (_nbSprite also bobs: PickupAnimationScript writes its localPosition.y, so only the
      // horizontal offset is fixed; the point is that it sank out of sight with its notebook)
      check("hier-used-notebook-art-hides", fabsf(nNow.x - nOff.x) < 1e-3f && fabsf(nNow.z - nOff.z) < 1e-3f &&
            ns->position.y < -10.0f,
            "_nbSprite y " + f2s(ns->position.y) + " after its notebook sank to -20");
      baldi->transform->position = bHome;
      nb->transform->position = nHome;
      eng::runFullFrame(1.0f / 50.0f);
    } else {
      fail("hier-character-sprite-follows", "Baldi/BaldiSprite or GeographyNotebook/_nbSprite not found");
    }
    // PickupAnimationScript: itemPosition->localPosition = (0, sin(frame deg)/2 + 1, 0) -> must bob.
    bb::PickupAnimationScript* pa = findAnyIncludingInactive<bb::PickupAnimationScript>();
    if (pa && pa->itemPosition && pa->gameObject && pa->gameObject->activeSelf) {
      float lo = 1e9f, hi = -1e9f;
      for (int f = 0; f < 60; f++) {
        eng::runFullFrame(1.0f / 50.0f);
        lo = (std::min)(lo, pa->itemPosition->position.y);
        hi = (std::max)(hi, pa->itemPosition->position.y);
      }
      check("hier-pickup-bobs", hi - lo > 0.3f,
            "pickup world y ranged only " + f2s(hi - lo) + " over 60 frames (localPosition writes ignored)");
    } else {
      fail("hier-pickup-bobs", "no active PickupAnimationScript with itemPosition");
    }
  }


  // NAVMESH (user: "characters are not properly following their AI path"; "Baldi chases me high in
  // the air floating"). Agents walk the GENUINE navmesh decoded from Scene/School/NavMesh.asset.
  {
    eng::State& gs = eng::state();
    // Agent height = surface + baseOffset * scale.y must reproduce every School agent's AUTHORED
    // height (they were placed by Unity's own agent): 2.017 at scale 2, Baldi 1.617 at scale 1.6.
    {
      int n = 0, bad = 0;
      std::string first;
      for (size_t k = 0; k < gs.agents.size(); k++) {
        bb::NavMeshAgent* a = gs.agents[k];
        if (!a->transform || gs.navPolys.empty()) continue;
        const float want = a->transform->position.y;   // authored world height at load
        const float got = gs.navPolys[0].centre.y + a->baseOffset * fabsf(a->transform->scale.y);
        n++;
        if (fabsf(want - got) > 0.01f) { bad++; if (first.empty()) first = a->gameObject->name + " authored " + f2s(want) + " vs placed " + f2s(got); }
      }
      check("nav-agent-height-matches-authored", n > 0 && bad == 0, i2s(bad) + " of " + i2s(n) + " agents: " + first);
    }
    check("nav-mesh-loaded", gs.navPolys.size() > 1000, i2s((long long)gs.navPolys.size()) + " navmesh polygons loaded (School has 1101)");
    struct NavT {
      static bool onMesh(float x, float z) {
        const std::vector<eng::NavPoly>& P = eng::state().navPolys;
        for (size_t i = 0; i < P.size(); i++) {
          bool pos = false, neg = false;
          for (size_t k = 0; k < P[i].v.size(); k++) {
            const bb::Vec3& a = P[i].v[k];
            const bb::Vec3& b = P[i].v[(k + 1) % P[i].v.size()];
            const float cr = (b.x - a.x) * (z - a.z) - (b.z - a.z) * (x - a.x);
            if (cr > 0.02f) pos = true; else if (cr < -0.02f) neg = true;
          }
          if (!(pos && neg)) return true;
        }
        return false;
      }
    };
    // A PROBE agent with School's agent settings (baseOffset 1, angularSpeed 360) and no script
    // driving it: Baldi's own AI would override destination and speed every frame.
    if (!gs.navPolys.empty()) {
      const bb::Vec3 home = gs.navPolys[0].centre + bb::Vec3(0.0f, 1.0f, 0.0f);
      bb::GameObject* probe = makeProbeObject("ProbeNavAgent", home);
      bb::NavMeshAgent* ag = probe->AddComponent<bb::NavMeshAgent>();
      ag->baseOffset = 1.0f;
      ag->angularSpeed = 360.0f;
      ag->acceleration = 1000.0f;   // School's agents (1st Prize aside)
      ag->speed = 40.0f;
      const float savedScale = bb::Time::timeScale;
      // a goal on the far side of the school: the polygon centre farthest from the start
      bb::Vec3 goal = home;
      float far = 0.0f;
      for (size_t k = 0; k < gs.navPolys.size(); k++) {
        const float d = bb::length(gs.navPolys[k].centre - home);
        if (d > far) { far = d; goal = gs.navPolys[k].centre; }
      }
      ag->SetDestination(goal);
      eng::runFullFrame(1.0f / 50.0f);                         // first step plans the path
      int segOff = 0, segSamples = 0;
      {
        const eng::AgentSim& sim = eng::state().agentSim[ag];
        bb::Vec3 a0 = probe->transform->position;
        for (size_t c = sim.corner; c < sim.corners.size(); c++) {
          const bb::Vec3 a1 = sim.corners[c];
          for (int k = 0; k <= 20; k++) {                     // the PATH itself must not cross a wall
            const bb::Vec3 q = a0 + (a1 - a0) * (k / 20.0f);
            segSamples++;
            if (!NavT::onMesh(q.x, q.z)) segOff++;
          }
          a0 = a1;
        }
        check("nav-planned-path-on-mesh", segSamples > 20 && segOff == 0,
              i2s(segOff) + " of " + i2s(segSamples) + " points on the planned corner path lie off the navmesh (" +
              i2s((long long)sim.corners.size()) + " corners)");
      }
      float maxDy = 0.0f, travelled = 0.0f;
      int offMesh = 0, samples = 0;
      bb::Vec3 prev = probe->transform->position;
      for (int f = 0; f < 1500; f++) {
        eng::runFullFrame(1.0f / 50.0f);
        const bb::Vec3 q0 = probe->transform->position;
        maxDy = (std::max)(maxDy, fabsf(q0.y - (gs.navPolys[0].centre.y + ag->baseOffset * fabsf(probe->transform->scale.y))));
        // the agent's actual position must be ON the navmesh every step (a chord between two steps
        // can shave a corner the agent turned within one step, so chords are not tested)
        samples++;
        if (!NavT::onMesh(q0.x, q0.z)) offMesh++;
        travelled += bb::length(q0 - prev);
        prev = q0;
        if (bb::length(bb::Vec3(q0.x - goal.x, 0.0f, q0.z - goal.z)) < 0.5f) break;
      }
      const float left = bb::length(bb::Vec3(prev.x - goal.x, 0.0f, prev.z - goal.z));
      check("nav-path-stays-on-mesh", samples > 0 && offMesh == 0,
            i2s(offMesh) + " of " + i2s(samples) + " sampled positions along the walk were OFF the navmesh (through a wall)");
      check("nav-agent-on-surface", maxDy < 0.05f,
            "agent height left surface + baseOffset by " + f2s(maxDy) + " (floating)");
      // ACCELERATION: from rest at acceleration 10 (1st Prize) the agent covers v*t - only
      // 0.5*a*t^2 in its first second (5 units), not speed*t.
      {
        bb::Vec3 st = probe->transform->position;
        ag->speed = 0.0f;                                    // come to rest (at 1000: one frame)
        for (int f = 0; f < 3; f++) eng::runFullFrame(1.0f / 50.0f);
        ag->acceleration = 10.0f;
        ag->SetDestination(home);
        st = probe->transform->position;
        ag->speed = 40.0f;
        for (int f = 0; f < 50; f++) eng::runFullFrame(1.0f / 50.0f);   // 1 s
        const float moved1s = bb::length(bb::Vec3(probe->transform->position.x - st.x, 0.0f, probe->transform->position.z - st.z));
        check("nav-agent-acceleration", moved1s > 4.0f && moved1s < 6.5f,
              "from rest at acceleration 10 the agent covered " + f2s(moved1s) + " in 1 s (0.5*10*1^2 = 5; instant would be 40)");
        ag->acceleration = 1000.0f;
        ag->SetDestination(goal);
      }
      check("nav-agent-reaches-goal", left < 0.5f && travelled >= far,
            "after the walk the agent was " + f2s(left) + " from a goal " + f2s(far) + " away (straight line), path length " + f2s(travelled));
      // FREEZE during a real PAUSE (GameControllerScript.PauseGame sets timeScale 0; the ThinkPad does
      // the same while learningActive). Unity agents advance on scaled time.
      if (gc) {
        ag->SetDestination(home);
        eng::runFullFrame(1.0f / 50.0f);
        gc->PauseGame();
        const bb::Vec3 frozenAt = probe->transform->position;
        float dtSeen = -1.0f;
        for (int f = 0; f < 50; f++) { eng::runFullFrame(1.0f / 50.0f); dtSeen = bb::Time::deltaTime(); }
        const float drift = bb::length(probe->transform->position - frozenAt);
        check("world-freezes-when-paused", drift < 1e-4f && bb::Time::timeScale == 0.0f,
              "while paused (timeScale " + f2s(bb::Time::timeScale) + ") the agent moved " + f2s(drift) +
              " in 50 frames (deltaTime after the frame " + f2s(dtSeen) + ")");
        gc->UnpauseGame();
      }
      bb::Time::timeScale = savedScale;
      eng::destroyGameObjectNow(probe);
      eng::runFullFrame(1.0f / 50.0f);
    } else {
      fail("nav-path-stays-on-mesh", "no navmesh");
    }
  }

  // iOS TOUCH INPUT (genuine iOS data: ios-ref/ios-mobilecontrols.json SimpleTouch layout at a 1:1
  // canvas scale on a 1280x720 screen; element ids + element->action map from data/rewired-touch.json).
  {
    eng::State& gs = eng::state();
    const bool wasMobile = gs.mobile;
    gs.mobile = true;
    const float H = 720.0f, W = 1280.0f;
    std::vector<eng::TouchControlDef> L;
    eng::TouchControlDef c;
    c.kind = eng::TouchControlDef::Pad;      c.x0 = W - 1600; c.x1 = W; c.y0 = H * 0.5f - 800; c.y1 = H * 0.5f + 800; c.elemX = 2; L.push_back(c);
    c = eng::TouchControlDef(); c.kind = eng::TouchControlDef::Blocker; c.x0 = 0; c.x1 = 200; c.y0 = H * 0.5f - 800; c.y1 = H * 0.5f + 800; L.push_back(c);
    c = eng::TouchControlDef(); c.kind = eng::TouchControlDef::Joystick; c.x0 = 25; c.x1 = 175; c.y0 = H - 175; c.y1 = H - 25; c.elemX = 0; c.elemY = 1; c.rangePx = 60; L.push_back(c);
    c = eng::TouchControlDef(); c.kind = eng::TouchControlDef::Button; c.x0 = W - 100; c.x1 = W - 25; c.y0 = H - 100; c.y1 = H - 25; c.elemButton = 3; L.push_back(c);   // Interact
    eng::touchSetLayout(L);
    eng::touchSetDpi(96.0f);
    bb::RewiredPlayer& rp = bb::ReInputPlayer();
    const float jcx = 100.0f, jcy = H - 100.0f;
    // joystick: 30 px right of centre = half range -> Strafe 0.5; far up-right clamps to the unit circle
    eng::touchDown(1, jcx + 30.0f, jcy);
    eng::rewiredBeginFrame();
    const float strafeHalf = rp.GetAxis("Strafe"), fwd0 = rp.GetAxis("Forward");
    eng::touchMove(1, jcx + 120.0f, jcy - 120.0f);
    eng::rewiredBeginFrame();
    const float sx = rp.GetAxis("Strafe"), sy = rp.GetAxis("Forward");
    eng::touchUp(1);
    eng::rewiredBeginFrame();
    check("ios-touch-joystick", fabsf(strafeHalf - 0.5f) < 1e-4f && fabsf(fwd0) < 1e-4f &&
          fabsf(sx - 0.70711f) < 1e-3f && fabsf(sy - 0.70711f) < 1e-3f && rp.GetAxis("Strafe") == 0.0f,
          "half deflection Strafe " + f2s(strafeHalf) + " Forward " + f2s(fwd0) + "; clamped (" + f2s(sx) + "," + f2s(sy) +
          ") expected (0.707,0.707); released " + f2s(rp.GetAxis("Strafe")));
    // pad: 48 px right in one frame at 96 dpi = half an inch = 50; a still frame = 0
    eng::touchDown(2, 800.0f, 300.0f);
    eng::rewiredBeginFrame();
    eng::touchMove(2, 848.0f, 300.0f);
    eng::rewiredBeginFrame();
    const float turnMoved = rp.GetAxis("Turn");
    eng::rewiredBeginFrame();
    const float turnStill = rp.GetAxis("Turn");
    eng::touchUp(2);
    eng::touchSetDpi(0.0f);                     // Screen.dpi unknown (< 10) -> Rewired assumes 96
    eng::touchDown(2, 800.0f, 300.0f); eng::rewiredBeginFrame();
    eng::touchMove(2, 848.0f, 300.0f); eng::rewiredBeginFrame();
    const float turnFallback = rp.GetAxis("Turn");
    eng::touchUp(2);
    eng::touchSetDpi(96.0f);
    check("ios-touch-pad-delta", fabsf(turnMoved - 50.0f) < 1e-3f && turnStill == 0.0f && fabsf(turnFallback - 50.0f) < 1e-3f &&
          rp.GetAxisCoordinateMode("Turn") == bb::AxisCoordinateMode::Absolute,
          "48 px at 96 dpi -> Turn " + f2s(turnMoved) + " (expect 50), still frame " + f2s(turnStill) +
          ", dpi fallback " + f2s(turnFallback));
    // button: down edge once, held while the touch lives even outside, Interact ALSO drives Jump
    eng::touchDown(3, W - 60.0f, H - 60.0f);
    eng::rewiredBeginFrame();
    const bool downEdge = rp.GetButtonDown("Interact") && rp.GetButtonDown("Jump");
    eng::touchMove(3, 600.0f, 300.0f);          // slides off the button
    eng::rewiredBeginFrame();
    const bool heldOut = rp.GetButton("Interact") && !rp.GetButtonDown("Interact");
    eng::touchUp(3);
    eng::rewiredBeginFrame();
    const bool released = !rp.GetButton("Interact");
    eng::touchDown(4, 600.0f, 300.0f);          // starts on the pad, slides ONTO the button
    eng::touchMove(4, W - 60.0f, H - 60.0f);
    eng::rewiredBeginFrame();
    const bool swipeIn = rp.GetButton("Interact");
    eng::touchUp(4);
    eng::touchDown(5, 100.0f, 100.0f);          // the left dead strip (Touch Pad_Dum) absorbs
    eng::touchMove(5, 160.0f, 100.0f);
    eng::rewiredBeginFrame();
    const float deadTurn = rp.GetAxis("Turn");
    eng::touchUp(5);
    eng::rewiredBeginFrame();
    check("ios-touch-buttons", downEdge && heldOut && released && !swipeIn && deadTurn == 0.0f,
          std::string("down edge (Interact+Jump) ") + (downEdge ? "ok" : "MISSING") + ", held after swipe-out " +
          (heldOut ? "ok" : "NO") + ", released " + (released ? "ok" : "NO") + ", swipe-in pressed " +
          (swipeIn ? "YES (wrong)" : "no") + ", dead strip turn " + f2s(deadTurn));

    // iOS PlayerScript in the real School: half joystick forward walks at FULL walkSpeed unless the
    // AnalogMove pref is 1 (sensitivityActive), then at half; a swipe turns by the decoded formula.
    bb::GameObject* pgo = 0;
    for (size_t i = 0; i < gs.gos.size(); i++) if (gs.gos[i]->name == "Player") pgo = gs.gos[i];
    bb::PlayerScript* ps = pgo ? pgo->GetComponent<bb::PlayerScript>() : 0;
    if (ps && gc && !gc->gamePaused) {
      const bb::Vec3 home = pgo->transform->position;
      const bool sa0 = ps->sensitivityActive;
      const float savedScale = bb::Time::timeScale;
      float dist[2];
      for (int mode = 0; mode < 2; mode++) {
        ps->sensitivityActive = mode == 1;
        pgo->transform->position = home;
        eng::touchDown(1, jcx, jcy - 30.0f);     // half deflection, straight up = Forward 0.5
        eng::runFullFrame(1.0f / 50.0f);
        const bb::Vec3 a = pgo->transform->position;
        for (int f = 0; f < 10; f++) eng::runFullFrame(1.0f / 50.0f);
        const bb::Vec3 b = pgo->transform->position;
        dist[mode] = bb::length(bb::Vec3(b.x - a.x, 0.0f, b.z - a.z));
        eng::touchUp(1);
        eng::runFullFrame(1.0f / 50.0f);
      }
      ps->sensitivityActive = sa0;
      pgo->transform->position = home;
      const float full = ps->walkSpeed * 10.0f / 50.0f;     // 10 frames at walkSpeed
      check("ios-player-analog-move", fabsf(dist[0] - full) < 0.05f * full && fabsf(dist[1] - 0.5f * full) < 0.05f * full,
            "half stick, 10 frames: AnalogMove off " + f2s(dist[0]) + " (expect " + f2s(full) + "), on " + f2s(dist[1]) +
            " (expect " + f2s(0.5f * full) + ")");
      // turn: one frame moving 6 px at 96 dpi = Turn 6.25 -> 6.25 * sens * timeScale * flipaturn * dt * 200
      // = 50 deg at the iOS TurnSlider default 2 (level1 Slider m_Value). Sensitivity is SET: the sim
      // has no MouseSensitivity pref, and 0 made an earlier version of this check pass vacuously.
      const float sens0 = ps->mouseSensitivity;
      ps->mouseSensitivity = 2.0f;
      const float yaw0 = ps->transform->rotation.eulerAngles().y;
      eng::touchDown(2, 800.0f, 300.0f);
      eng::runFullFrame(1.0f / 50.0f);
      const float yawA = ps->transform->rotation.eulerAngles().y;
      eng::touchMove(2, 806.0f, 300.0f);
      eng::runFullFrame(1.0f / 50.0f);
      const float yawB = ps->transform->rotation.eulerAngles().y;
      eng::touchUp(2);
      eng::runFullFrame(1.0f / 50.0f);
      float got = fmodf(yawB - yawA + 720.0f, 360.0f);
      if (got > 180.0f) got -= 360.0f;
      const float want = fmodf(6.25f * ps->mouseSensitivity * ps->flipaturn * (1.0f / 50.0f) * 200.0f + 720.0f, 360.0f);
      float w2 = want > 180.0f ? want - 360.0f : want;
      ps->mouseSensitivity = sens0;
      check("ios-player-touch-turn", fabsf(w2) > 1.0f && fabsf(got - w2) < 0.05f && fabsf(fmodf(yawA - yaw0 + 360.0f, 360.0f)) < 1e-3f,
            "6 px swipe turned " + f2s(got) + " deg (iOS formula: 6.25 * sens " + f2s(ps->mouseSensitivity) +
            " * dt * 200 = " + f2s(w2) + "); touch-down frame turned " + f2s(yawA - yaw0));
      bb::Time::timeScale = savedScale;
    } else {
      fail("ios-player-analog-move", "no Player/PlayerScript or game paused");
    }
    eng::touchSetLayout(std::vector<eng::TouchControlDef>());
    gs.mobile = wasMobile;
  }

  // APP SUSPEND (iOS): OnApplicationPause -> GameControllerScript pauses the game (iOS binary:
  // `if (!gamePaused) PauseGame();`, pauseStatus ignored -- resume keeps it paused).
  if (gc && gc->pauseMenu && !gc->gamePaused) {
    const float savedScale = bb::Time::timeScale;
    eng::applicationPause(true);
    const bool pausedOnSuspend = gc->gamePaused && gc->pauseMenu->activeSelf && bb::Time::timeScale == 0.0f;
    eng::applicationPause(false);
    const bool stillPausedOnResume = gc->gamePaused && gc->pauseMenu->activeSelf;
    check("ios-app-suspend-pauses-game", pausedOnSuspend && stillPausedOnResume,
          std::string("suspend: ") + (pausedOnSuspend ? "paused" : "NOT paused") + ", resume: " +
          (stillPausedOnResume ? "still paused (as the iOS build)" : "unpaused"));
    gc->UnpauseGame();
    bb::Time::timeScale = savedScale;
  } else {
    fail("ios-app-suspend-pauses-game", "no GameControllerScript / pauseMenu, or already paused");
  }

  // PAUSE MENU (user: "the in-game menu that appears when you press escape isn't implemented").
  // GameControllerScript.Update: PauseOrCancel (Escape) -> PauseGame (timeScale 0, pauseMenu on);
  // Escape again -> UnpauseGame.
  if (gc && gc->pauseMenu) {
    const float savedScale = bb::Time::timeScale;
    bb::Time::timeScale = 1.0f;
    eng::injectKey((int)bb::KeyCode::Escape, true);
    eng::runFullFrame(1.0f / 50.0f);
    eng::injectKeyRelease((int)bb::KeyCode::Escape);
    eng::runFullFrame(1.0f / 50.0f);
    const bool paused = gc->gamePaused && gc->pauseMenu->activeSelf && bb::Time::timeScale == 0.0f;
    eng::injectKey((int)bb::KeyCode::Escape, true);
    eng::runFullFrame(1.0f / 50.0f);
    eng::injectKeyRelease((int)bb::KeyCode::Escape);
    eng::runFullFrame(1.0f / 50.0f);
    check("pause-menu-opens-on-escape", paused, "Escape did not pause (gamePaused / pauseMenu / timeScale 0)");
    check("pause-menu-closes-on-escape", !gc->gamePaused && !gc->pauseMenu->activeSelf,
          "a second Escape did not resume");
    bb::Time::timeScale = savedScale;
  }

  // PLAY ON AWAKE on activation (user: Playtime's music / the robot's motor never play). Both loops
  // sit on objects inactive at load that ActivateSpoopMode switches on; Unity starts a Play On Awake
  // source when it becomes active and stops it when deactivated.
  {
    bb::GameObject* pt = 0;
    for (size_t i = 0; i < eng::state().gos.size(); i++) if (eng::state().gos[i]->name == "Playtime") pt = eng::state().gos[i];
    bb::AudioSource* music = 0;
    if (pt) for (size_t c = 0; c < pt->components.size(); c++) {
      bb::AudioSource* a = dynamic_cast<bb::AudioSource*>(pt->components[c]);
      if (a && a->playOnAwake && a->clip) music = a;
    }
    if (pt && music && !pt->activeSelf) {
      pt->SetActive(true);
      const bool on = music->isPlaying();
      pt->SetActive(false);
      check("audio-play-on-awake-when-activated", on && !music->isPlaying(),
            std::string("Playtime's music ") + (on ? "started" : "did NOT start") + " on SetActive(true) and " +
            (music->isPlaying() ? "kept playing" : "stopped") + " on SetActive(false)");
    } else {
      fail("audio-play-on-awake-when-activated", "Playtime / her music source not found (or already active)");
    }
  }

  // ITEM SELECTOR, by GameControllerScript.cs: CollectItem fills the first empty slot (texture =
  // itemTextures[id]); Increase/DecreaseItemSelection wrap 0..2 and move the red ItemSelect to
  // itemSelectOffset[sel]; UpdateItemName shows itemNames[item[sel]]; UseItem(1) (Zesty Bar) sets
  // stamina = maxStamina * 2 and ResetItem clears the slot back to itemTextures[0] / "Nothing".
  if (gc && gc->itemSelect && gc->itemText && gc->itemSlot[0] && gc->itemSlot[1] && gc->itemSlot[2] && player) {
    check("items-select-offsets-decoded", gc->itemSelectOffset[0] == -80 && gc->itemSelectOffset[1] == -40 &&
          gc->itemSelectOffset[2] == 0,
          "itemSelectOffset = {" + i2s(gc->itemSelectOffset[0]) + "," + i2s(gc->itemSelectOffset[1]) + "," +
          i2s(gc->itemSelectOffset[2]) + "}, scene packs {-80,-40,0}");
    int savedItems[3] = { gc->item[0], gc->item[1], gc->item[2] };
    const int savedSel = gc->itemSelected;
    const float savedStamina = player->stamina;
    gc->item[0] = gc->item[1] = gc->item[2] = 0;
    gc->itemSelected = 0;
    gc->CollectItem(1);                                        // Energy flavored Zesty Bar
    gc->CollectItem(10);                                       // Big Ol' Boots (id 10: needs a 12-entry table)
    check("items-collect-fills-slots", gc->item[0] == 1 && gc->item[1] == 10 &&
          gc->itemSlot[0]->texture == gc->itemTextures[1] && gc->itemTextures[1] != 0 &&
          gc->itemSlot[1]->texture == gc->itemTextures[10] && gc->itemTextures[10] != 0,
          "slots " + i2s(gc->item[0]) + "," + i2s(gc->item[1]) + "; boots texture " +
          (gc->itemTextures[10] ? "bound" : "NULL"));
    check("items-name-shows-selected", gc->itemText->GetText() == "Energy flavored Zesty Bar",
          "item label reads '" + gc->itemText->GetText() + "'");
    const float x0 = gc->itemSelect->anchoredPosition.x;
    gc->IncreaseItemSelection();
    const float x1 = gc->itemSelect->anchoredPosition.x;
    const std::string n1 = gc->itemText->GetText();
    gc->IncreaseItemSelection();
    const float x2 = gc->itemSelect->anchoredPosition.x;
    gc->IncreaseItemSelection();                               // wraps 2 -> 0
    const float x3 = gc->itemSelect->anchoredPosition.x;
    gc->DecreaseItemSelection();                               // wraps 0 -> 2
    const float x4 = gc->itemSelect->anchoredPosition.x;
    check("items-selector-moves-and-wraps", x0 == -80.0f && x1 == -40.0f && x2 == 0.0f && x3 == -80.0f &&
          x4 == 0.0f && n1 == "Big Ol' Boots",
          "selector x: " + f2s(x0) + " " + f2s(x1) + " " + f2s(x2) + " " + f2s(x3) + " " + f2s(x4) +
          ", slot-2 name '" + n1 + "'");
    gc->DecreaseItemSelection();
    gc->DecreaseItemSelection();                               // back on slot 0 (the Zesty Bar)
    gc->UseItem();
    check("items-zesty-bar-used", gc->itemSelected == 0 && player->stamina == player->maxStamina * 2.0f &&
          gc->item[0] == 0 && gc->itemSlot[0]->texture == gc->itemTextures[0] &&
          gc->itemText->GetText() == "Nothing",
          "after UseItem: stamina " + f2s(player->stamina) + " (max " + f2s(player->maxStamina) + "), slot0 id " +
          i2s(gc->item[0]) + ", label '" + gc->itemText->GetText() + "'");
    for (int k = 0; k < 3; k++) {
      gc->item[k] = savedItems[k];
      gc->itemSlot[k]->texture = gc->itemTextures[savedItems[k]];
    }
    // The WHEEL path, through Input.GetAxis("Mouse ScrollWheel") in a real frame (the shell feeds
    // notches * 0.1, InputManager.asset's sensitivity): down (< 0) = next slot, up (> 0) = previous.
    {
      const float savedScale = bb::Time::timeScale;
      bb::Time::timeScale = 1.0f;                              // GameController ignores the wheel at 0
      gc->itemSelected = 0;
      eng::injectAxis("Mouse ScrollWheel", -0.1f);
      eng::runFullFrame(1.0f / 50.0f);
      eng::injectAxis("Mouse ScrollWheel", 0.0f);
      const int selDown = gc->itemSelected;
      const float xDown = gc->itemSelect->anchoredPosition.x;
      eng::injectAxis("Mouse ScrollWheel", 0.1f);
      eng::runFullFrame(1.0f / 50.0f);
      eng::injectAxis("Mouse ScrollWheel", 0.0f);
      check("items-scroll-wheel-selects", selDown == 1 && xDown == -40.0f && gc->itemSelected == 0 &&
            gc->itemSelect->anchoredPosition.x == -80.0f,
            "one notch down -> slot " + i2s(selDown) + " (x " + f2s(xDown) + "), one notch up -> slot " +
            i2s(gc->itemSelected));
      bb::Time::timeScale = savedScale;
    }
    gc->itemSelected = savedSel;
    player->stamina = savedStamina;
  } else {
    fail("items-select-offsets-decoded", "GameController item HUD references not bound");
  }

  // DESKS (user report: big desks "facing about 90 degrees to the right or left"). Every real
  // mesh whose PARENT carries a BoxCollider must cover the same world footprint as that collider
  // -- in the game you collide with a desk exactly where you see it. Footprint = world half-extent
  // along world X and Z of (rotation, scale, local extent), for the mesh and the collider alike.
  {
    int pairs = 0, match = 0;
    std::string firstBad;
    std::vector<bb::MeshRenderer*>& all = eng::state().meshRenderers;   // what drawWorld draws
    for (size_t i = 0; i < all.size(); i++) {
      bb::MeshRenderer* mr = all[i];
      if (!mr || mr->meshPrimitive != 4300000 || mr->meshGuid.empty() || !mr->transform) continue;
      const eng::MeshAsset* ma = eng::meshForGuid(mr->meshGuid);
      bb::Transform* par = mr->transform->parent;
      if (!ma || !par || !par->go) continue;
      bb::BoxCollider* bc = par->go->GetComponent<bb::BoxCollider>();
      if (!bc) continue;
      float mx = 0.0f, mz = 0.0f, cx = 0.0f, cz = 0.0f;
      for (int k = 0; k < 3; k++) {
        bb::Vec3 e(k == 0 ? 1.0f : 0.0f, k == 1 ? 1.0f : 0.0f, k == 2 ? 1.0f : 0.0f);
        const float ms = (k == 0 ? mr->transform->scale.x : k == 1 ? mr->transform->scale.y : mr->transform->scale.z);
        const bb::Vec3 mv = mr->transform->rotation.rotate(e) * (fabsf(ms) * ma->extent[k]);
        mx += fabsf(mv.x); mz += fabsf(mv.z);
        const float cs = (k == 0 ? par->scale.x : k == 1 ? par->scale.y : par->scale.z);
        const float sz = (k == 0 ? bc->size.x : k == 1 ? bc->size.y : bc->size.z);
        const bb::Vec3 cv = par->rotation.rotate(e) * (fabsf(cs) * sz * 0.5f);
        cx += fabsf(cv.x); cz += fabsf(cv.z);
      }
      pairs++;
      // ORIENTATION is the defect being guarded: where either footprint is clearly oblong (>1.2:1)
      // the long axes must agree. Sizes only roughly (colliders are authored a little generous --
      // the chairs' are ~25% larger than the seat).
      const bool mLongX = mx > mz, cLongX = cx > cz;
      const bool oblong = (mx > 1.2f * mz || mz > 1.2f * mx) || (cx > 1.2f * cz || cz > 1.2f * cx);
      const bool ok = (!oblong || mLongX == cLongX) &&
                      fabsf(mx - cx) <= 0.35f * cx + 0.05f && fabsf(mz - cz) <= 0.35f * cz + 0.05f;
      if (ok) match++;
      else if (firstBad.empty()) {
        char buf[200];
        snprintf(buf, sizeof buf, "'%s' mesh %.2f x %.2f vs collider %.2f x %.2f",
                 mr->gameObject ? mr->gameObject->name.c_str() : "?",
                 2.0f * mx, 2.0f * mz, 2.0f * cx, 2.0f * cz);
        firstBad = buf;
      }
    }
    printf("DESKS mesh-vs-collider footprints: %d of %d match %s\n", match, pairs, firstBad.c_str());
    check("school-desk-mesh-on-collider", pairs > 0 && match == pairs,
          f2s((float)match) + " of " + f2s((float)pairs) + " meshes cover their collider -- first: " + firstBad);
  }


  // DOORS (user report: "i still cannot open doors"). DoorScript.Update opens a door only when
  // Physics.Raycast from the screen centre hits THAT door's `trigger` collider FIRST, within
  // openingDistance, unlocked. This casts at every School door's trigger from both sides and
  // reports the first collider hit, so the failing link is named instead of guessed.
  {
    std::vector<bb::DoorScript*> doors;
    {
      std::vector<bb::GameObject*>& all = eng::liveGameObjects();
      for (size_t i = 0; i < all.size(); i++)
        if (all[i]->activeSelf)
          if (bb::DoorScript* ds = all[i]->GetComponent<bb::DoorScript>()) doors.push_back(ds);
    }
    int ok = 0, noTrigger = 0, unsupported = 0, blockedBy = 0, missed = 0;
    std::string firstBlock;
    for (size_t i = 0; i < doors.size(); i++) {
      bb::DoorScript* d = doors[i];
      if (!d->trigger || !d->trigger->transform) { noTrigger++; continue; }
      if (d->trigger->surface == bb::MeshCollider::SurfaceNone) { unsupported++; continue; }
      const bb::Transform* tt = d->trigger->transform;
      const bb::Vec3 nrm = bb::normalized(tt->rotation.rotate(
          d->trigger->surface == bb::MeshCollider::SurfaceQuad ? bb::Vec3(0.0f, 0.0f, 1.0f)
                                                               : bb::Vec3(0.0f, 1.0f, 0.0f)));
      // BOTH sides must work: a classroom is left through the same door it was entered by.
      int sidesHit = 0;
      for (int side = 0; side < 2; side++) {
        const bb::Vec3 dir = side ? nrm : nrm * -1.0f;
        bb::Ray ray;
        ray.origin = tt->position - dir * 3.0f;
        ray.direction = dir;
        bb::RaycastHit h;
        if (bb::Physics::Raycast(ray, &h)) {
          if (h.collider == d->trigger) sidesHit++;
          else if (firstBlock.empty() && h.collider && h.collider->gameObject)
            firstBlock = "'" + d->gameObject->name + "' blocked by '" + h.collider->gameObject->name + "'";
        }
      }
      const bool hitIt = sidesHit == 2;
      if (hitIt) ok++;
      else {
        bb::Ray ray; ray.origin = tt->position - nrm * 3.0f; ray.direction = nrm;
        bb::RaycastHit h;
        if (bb::Physics::Raycast(ray, &h)) blockedBy++; else missed++;
      }
    }
    printf("DOORS total=%d rayHitsTrigger=%d noTrigger=%d unsupportedSurface=%d blocked=%d missed=%d %s\n",
           (int)doors.size(), ok, noTrigger, unsupported, blockedBy, missed, firstBlock.c_str());
    check("school-door-trigger-raycastable", !doors.empty() && ok == (int)doors.size(),
          f2s((float)ok) + " of " + f2s((float)doors.size()) + " doors can be hit by the open ray from BOTH sides");

    // END TO END: the real player stands 4 units out facing the door (alternate doors from
    // alternate sides), the camera follows through the game's own scripts, Mouse0 goes in
    // through the live input path, and DoorScript.Update must open the door by itself.
    if (player && player->transform) {
      bb::Transform* pt = player->transform;
      const bb::Vec3 home = pt->position;
      const bb::Quat homeRot = pt->rotation;
      int opened = 0, openSeeThrough = 0;
      std::string firstFail;
      for (size_t i = 0; i < doors.size(); i++) {
        bb::DoorScript* d = doors[i];
        if (!d->trigger || !d->trigger->transform) continue;
        const bb::Transform* tt = d->trigger->transform;
        bb::Vec3 dir = bb::normalized(tt->rotation.rotate(bb::Vec3(0.0f, 0.0f, 1.0f)));
        if (i % 2) dir = dir * -1.0f;
        dir.y = 0.0f;
        dir = bb::normalized(dir);
        bb::Vec3 at = tt->position - dir * 4.0f;
        at.y = home.y;
        pt->position = at;
        const float yaw = atan2f(dir.x, dir.z);
        pt->rotation = bb::Quat(0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f));
        // PlayerScript.MouseMove writes its private playerRotation over transform.rotation every
        // Update (PlayerScript.cs:50), so the facing has to be set THERE as well.
        player->playerRotation = pt->rotation;
        for (int f = 0; f < 3; f++) eng::runFullFrame(0.02f);
        eng::injectMouse(0, true);
        eng::runFullFrame(0.02f);
        eng::injectMouseRelease(0);
        eng::runFullFrame(0.02f);
        if (d->bDoorOpen) {
          opened++;
          // Open faces must be the authored Cutout materials, textured and alpha-tested (Door_80
          // and kin are Standard / Rendering Mode Cutout / _ALPHATEST_ON in the decompile).
          const bb::Material* mi = d->inside ? d->inside->material : 0;
          const bb::Material* mo = d->outside ? d->outside->material : 0;
          // ...and the texture's PIXELS must exist: naming a texture is not enough. The first
          // version of this check stopped at mainTexGuid and passed while every open door drew
          // the flat grey no-texture fallback, because the baker had never baked these textures.
          bool pixels = mi && mo;
          for (int f = 0; f < 2 && pixels; f++) {
            const bb::Material* mm = f ? mo : mi;
            FILE* tf = mm->mainTexGuid.empty() ? 0 : fopen(("data/tex/" + mm->mainTexGuid + ".raw").c_str(), "rb");
            if (tf) fclose(tf); else pixels = false;
          }
          if (pixels && mi->alphaTest && mo->alphaTest) openSeeThrough++;
        }
        else if (firstFail.empty()) {
          bb::Camera* cam = bb::Camera::main();
          const bb::Vec3 cf = cam && cam->transform
              ? cam->transform->rotation.rotate(bb::Vec3(0.0f, 0.0f, 1.0f)) : bb::Vec3();
          char buf[256];
          snprintf(buf, sizeof buf, "first failure '%s' side %d: locked=%d timeScale=%.2f camFwd.dir=%.3f dist=%.2f",
                   d->gameObject->name.c_str(), (int)(i % 2), (int)d->bDoorLocked, bb::Time::timeScale,
                   bb::dot(cf, dir), bb::dist(pt->position, d->transform->position));
          firstFail = buf;
        }
      }
      pt->position = home;
      pt->rotation = homeRot;
      player->playerRotation = homeRot;
      printf("DOORCLICK opened=%d of %d %s\n", opened, (int)doors.size(), firstFail.c_str());
      check("school-door-open-faces-cutout", opened > 0 && openSeeThrough == opened,
            f2s((float)openSeeThrough) + " of " + f2s((float)opened) +
            " opened doors show a textured, alpha-tested face (the rest draw solid)");
      check("school-door-click-opens", opened == (int)doors.size(),
            f2s((float)opened) + " of " + f2s((float)doors.size()) + " doors opened on a real click -- " + firstFail);
    }
  }

  // TEMPORARY walk-repro (BB_WALKREPRO): march the real player capsule into the nearest wall quad
  // frame by frame and print the signed plane distance each frame -- a stable run converges to a
  // constant stop distance; an oscillating run shows the jitter the user reported live.
  if (getenv("BB_WALKREPRO") && player && player->cc && player->cc->transform) {
    bb::CharacterController* wcc = player->cc;
    eng::State& gs = eng::state();
    const bb::Vec3 probe(5.0f, 1.0f, -5.0f);
    // Pick the wall quad by TRUE distance from the probe point to the panel's world OBB (extent
    // included) -- nearest infinite plane alone can select a parallel wall far down the hall
    // whose plane passes through here but whose face does not, and the walk then tunnels
    // through empty doorway space. Skip tiny quads (door headers, trim).
    bb::MeshCollider* best = 0;
    bb::Vec3 bCtr, bAxis[3], bestQ;
    float bHalf[3], bestD = 1e9f;
    for (size_t i = 0; i < gs.meshColliders.size(); i++) {
      bb::MeshCollider* m = gs.meshColliders[i];
      if (!m->transform || m->isTrigger || m->surface != bb::MeshCollider::SurfaceQuad) continue;
      const bb::Vec3 n = bb::normalized(m->transform->rotation.rotate(bb::Vec3(0.0f, 0.0f, 1.0f)));
      if (fabsf(n.y) > 0.5f) continue;               // walls only (not floors/ceilings)
      const bb::Vec3 s = m->transform->scale;
      const float hu = m->halfU * fabsf(s.x), hv = m->halfV * fabsf(s.y);
      if (hu < 1.5f || hv < 1.5f) continue;          // skip headers/trim
      bb::Vec3 ctr, axis[3]; float half[3];
      ctr = m->transform->position;
      axis[0] = m->transform->rotation.rotate(bb::Vec3(1.0f, 0.0f, 0.0f));
      axis[1] = n;                                   // panel normal (local +z) is axis 2 below
      axis[1] = m->transform->rotation.rotate(bb::Vec3(0.0f, 1.0f, 0.0f));
      axis[2] = n;
      half[0] = hu; half[1] = hv; half[2] = 0.0f;
      const bb::Vec3 d = probe - ctr;
      bb::Vec3 q = ctr;
      for (int a = 0; a < 3; a++) q = q + axis[a] * bb::clampf(bb::dot(axis[a], d), -half[a], half[a]);
      const float dist = bb::length(probe - q);
      if (dist < bestD) { best = m; bestD = dist; bCtr = ctr;
        for (int a = 0; a < 3; a++) bAxis[a] = axis[a];
        bHalf[0] = half[0]; bHalf[1] = half[1]; bHalf[2] = half[2]; bestQ = q; }
    }
    if (best) {
      bb::Transform* bt = best->transform;
      const bb::Vec3 n = bb::normalized(bt->rotation.rotate(bb::Vec3(0.0f, 0.0f, 1.0f)));
      // start 1.2 units off the panel surface, at the closest-point lateral position, y kept at
      // the capsule's walking height so the walk reproduces a real approach.
      bb::Vec3 start = bestQ + n * 1.2f;
      start.y = 1.0f;
      wcc->transform->position = start;
      eng::setDebugFlags(eng::kDebugDiag);                 // contacts print on the CCDBG cadence
      printf("WALK start pos=(%.3f,%.3f,%.3f) n=(%.3f,%.3f,%.3f) panel='%s' ctr=(%.2f,%.2f,%.2f)"
             " ccR=%.3f ccH=%.3f ccC=(%.2f,%.2f,%.2f)\n",
             start.x, start.y, start.z, n.x, n.y, n.z,
             best->gameObject ? best->gameObject->name.c_str() : "?",
             bt->position.x, bt->position.y, bt->position.z,
             wcc->radius, wcc->height, wcc->center.x, wcc->center.y, wcc->center.z);
      for (int f = 0; f < 120; f++) {
        wcc->Move(n * (-0.12f));                           // walk along -n into the wall
        const bb::Vec3 p = wcc->transform->position;
        const float planeDist = bb::dot(n, p - bt->position);
        // manual closest separation from the capsule axis mid to the panel OBB (what the sweep
        // compares against radius ~0.4): this is the value that must go <= radius and HOLD.
        const bb::Vec3 d = p - bCtr;
        bb::Vec3 q = bCtr;
        for (int a = 0; a < 3; a++) q = q + bAxis[a] * bb::clampf(bb::dot(bAxis[a], d), -bHalf[a], bHalf[a]);
        const float sep = bb::length(p - q);
        printf("WALK f=%d pos=(%.4f,%.4f,%.4f) planeDist=%.4f sep=%.4f\n",
               f, p.x, p.y, p.z, planeDist, sep);
      }
      eng::setDebugFlags(0);
      fflush(stdout);
    } else {
      printf("WALK repro: no wall quad near spawn found\n");
    }
  }

  // Baldi's GameObject is inactive at load, so this must NOT go through FindObjectOfType.
  check("school-baldi-inactive-at-load", bb::FindObjectOfType<bb::BaldiScript>() == 0,
        "FindObjectOfType found Baldi, but the bake marks his GameObject active:false");
  bb::BaldiScript* baldi = findAnyIncludingInactive<bb::BaldiScript>();
  check("school-baldi-present", baldi != 0, "no BaldiScript anywhere in the scene");
  if (baldi) {
    // speed is a serialized float (bake: 75); a zero means the Script field block never bound.
    check("school-baldi-speed-baked", baldi->speed == 75.0f,
          "BaldiScript::speed == " + f2s(baldi->speed) + " (bake says 75)");
    // Clip durations come from the real WAV's RIFF header, read at `assetRoot + <manifest rel>`.
    // A manifest rel that lost its directory still "works" -- parseWavDuration just cannot open
    // the file and returns its 1.0s fallback, silently giving every clip in the game the same
    // length. So assert a real, non-fallback duration: BAL_Slap.wav is ~0.5s, definitely not 1.0.
    // Expected value read straight off the file: AudioClip/Characters/Baldi/Sounds/BAL_Slap.wav
    // is mono 16-bit 44100Hz, 67584 data bytes / 88200 bytesPerSec = 0.766259s.
    if (baldi->slap) {
      const float kSlapSeconds = 0.766259f;
      float len = eng::clipLength(baldi->slap);
      check("audio-clip-duration-from-wav", fabsf(len - kSlapSeconds) < 1e-4f,
            "BAL_Slap.wav length came back " + f2s(len) + ", expected " + f2s(kSlapSeconds) +
            (len == 1.0f ? " (1.0 exactly = parseWavDuration's fallback: the WAV could not be "
                           "opened, so check that clipForGuid resolves the manifest rel and that "
                           "manifest rels still carry their directories)"
                         : " (RIFF parse disagrees with the file)"));
    } else {
      fail("audio-clip-duration-from-wav", "BaldiScript::slap clip did not bind");
    }

    check("school-baldi-refs-bound", baldi->player != 0 && baldi->wanderTarget != 0,
          std::string("player=") + (baldi->player ? "ok" : "null") +
          " wanderTarget=" + (baldi->wanderTarget ? "ok" : "null"));
    // agent is assigned in Start() via GetComponent -- and Baldi is INACTIVE, so Unity never ran
    // his Start(). A non-null agent here would mean the loader wrongly ran lifecycle on an
    // inactive GameObject (pass 4 is per-ACTIVE-GO by contract).
    check("school-baldi-start-skipped", baldi->agent == 0,
          "BaldiScript::agent is set, so Start() ran on an inactive GameObject");
  }

  if (player) {
    // staminaBar is a uGUI Slider. uGUI components serialize as MonoBehaviours under ONE
    // assembly guid, so the baker must identify the class by m_Script fileID -- when it mapped
    // that guid wholesale to "CanvasScaler", this field bound to nothing and StaminaCheck's
    // `staminaBar->value` dereferenced null on the very first Update.
    check("school-player-staminabar-bound", player->staminaBar != 0,
          "PlayerScript::staminaBar (uGUI Slider) stayed null");
    check("school-player-hud-bound", player->hud != 0, "PlayerScript::hud (Canvas) stayed null");
  }

  // The uGUI EventSystem is a MonoBehaviour too, so it also arrives via the Script branch.
  check("school-eventsystem-registered", bb::EventSystem::current() != 0,
        "EventSystem::current() null -- the loader never registered the scene's EventSystem");

  if (gc) {
    // Prefab-typed GameObject fields: these serialize as {fileID, guid} and resolve to inert
    // template shells via the bindGameObject guid fallback. GC's UseItem path dereferences them.
    check("school-gc-prefab-refs", gc->bsodaSpray != 0 && gc->alarmClock != 0 && gc->quarter != 0,
          std::string("bsodaSpray=") + (gc->bsodaSpray ? "ok" : "null") +
          " alarmClock=" + (gc->alarmClock ? "ok" : "null") +
          " quarter=" + (gc->quarter ? "ok" : "null"));
    check("school-gc-player-ref", gc->player != 0, "GameControllerScript::player null");
  }
  return player;
}

// ---- raycast assertions -----------------------------------------------------------------------
// Uses the Player's CharacterController shell capsule as the target: it is registered by the
// loader via noteComponent() (deliberately absent from go->components), and the scripts' sight
// checks all depend on it being hittable.
void testRaycast(bb::PlayerScript* player) {
  eng::State& g = eng::state();
  if (!player || !player->cc || !player->cc->transform) {
    fail("raycast-player-hit", "no player CharacterController to aim at");
    fail("raycast-origin-inside-skips", "no player CharacterController to aim at");
    return;
  }
  // The Player GameObject carries TWO capsules: the one authored in the scene (bake fileID 20532,
  // present in go->components) and the shell the loader mirrors from the CharacterController so
  // the player is raycast-hittable. Per the loader contract the shell is registered for queries
  // via noteComponent() but deliberately kept OUT of go->components, so that Unity's
  // GetComponent<CapsuleCollider>() keeps returning the authored collider.
  bb::GameObject* pgo = player->cc->gameObject;
  std::vector<bb::CapsuleCollider*> onPlayer;
  for (size_t i = 0; i < g.capsuleColliders.size(); i++) {
    if (g.capsuleColliders[i]->gameObject == pgo) onPlayer.push_back(g.capsuleColliders[i]);
  }
  check("raycast-cc-shell-registered", onPlayer.size() == 2,
        i2s((long long)onPlayer.size()) + " capsules registered on the Player (expected 2: the "
        "authored collider + the CharacterController shell)");

  bb::CapsuleCollider* authored = pgo->GetComponent<bb::CapsuleCollider>();
  check("cc-shell-not-in-components", authored != 0 && onPlayer.size() == 2 &&
        (onPlayer[0] == authored) != (onPlayer[1] == authored),
        "GetComponent<CapsuleCollider>() must return the authored collider and exactly one of the "
        "two registered capsules must be absent from go->components");

  // Aim at the shell specifically (the capsule that exists so scripts can see the player).
  bb::CapsuleCollider* shell = 0;
  for (size_t i = 0; i < onPlayer.size(); i++) if (onPlayer[i] != authored) shell = onPlayer[i];
  if (!shell) { fail("raycast-player-hit", "no CharacterController shell capsule"); return; }

  const bb::Vec3 center = player->cc->transform->position + shell->center;

  // Straight down from above the capsule: must hit, and a true return must carry a collider
  // (the engine contract forbids found-with-null-collider; every script derefs hit.collider).
  bb::RaycastHit hit;
  bool got = bb::Physics::Raycast(center + bb::Vec3(0.0f, 5.0f, 0.0f), bb::Vec3(0.0f, -1.0f, 0.0f),
                                  &hit, 20.0f, ~0, bb::QueryTriggerInteraction::Collide);
  if (!got) {
    fail("raycast-player-hit", "downward cast from 5u above the player capsule missed");
  } else if (!hit.collider) {
    fail("raycast-player-hit", "hit reported with a null collider (contract violation)");
  } else {
    pass("raycast-player-hit");
  }

  // Origin inside a shape -> that shape is skipped (-2 convention, eng-core). Casting from the
  // capsule's own center must therefore never return the capsule itself.
  bb::RaycastHit inner;
  bool gotInner = bb::Physics::Raycast(center, bb::Vec3(0.0f, 1.0f, 0.0f), &inner, 0.25f, ~0,
                                       bb::QueryTriggerInteraction::Collide);
  bool selfHit = gotInner && inner.collider == (bb::Collider*)shell;
  check("raycast-origin-inside-skips", !selfHit,
        "cast from inside the capsule returned the capsule itself");

  // Empty-space cast: a finite ray far outside the level crosses nothing, so this must be false
  // (and must not write through `hit`).
  // NOTE: maxDistance 0 is NOT a miss here -- eng-core treats `maxDistance <= 0` as INFINITY, the
  // documented Unity convention that Physics::Raycast(ray, hit) relies on (it forwards 0.0f to
  // mean unbounded). So the miss case has to be geometric, not a zero-length ray.
  bb::RaycastHit none;
  bool gotNone = bb::Physics::Raycast(bb::Vec3(1.0e5f, 1.0e5f, 1.0e5f), bb::Vec3(0.0f, 1.0f, 0.0f),
                                      &none, 1.0f, ~0, bb::QueryTriggerInteraction::Collide);
  check("raycast-empty-space-misses", !gotNone, "a 1u ray at (1e5,1e5,1e5) reported a hit");
  check("raycast-miss-leaves-hit-clean", !gotNone && none.collider == 0,
        "a missed cast wrote a collider into the hit struct");
}

// ---- deep prefab cloning ----------------------------------------------------------------------
// Instantiate() used to return a shallow inert named shell with no components and no lifecycle.
// It now instantiates data/prefab-<Name>.bake.json through the same 4 passes the scene loader
// uses, so a clone must come back with real components and bound fields. GC's item chain
// dereferences these (UseItem item-1 throws BSODA spray, item-7 drops the alarm clock).
void testPrefabCloning() {
  bb::GameControllerScript* gc = bb::FindObjectOfType<bb::GameControllerScript>();
  if (!gc || !gc->bsodaSpray) {
    fail("prefab-clone-has-components", "no GameControllerScript::bsodaSpray template to clone");
    return;
  }
  eng::State& g = eng::state();
  const size_t before = g.gos.size();

  // The template shell itself is inert by design and must stay out of the scene registry
  // (GameObject.Find must not see a prefab template).
  check("prefab-template-is-inert", gc->bsodaSpray->components.empty(),
        i2s((long long)gc->bsodaSpray->components.size()) +
        " components on the inert template shell (expected 0)");

  const bb::Vec3 at(12.5f, 3.0f, -7.25f);
  bb::GameObject* clone = bb::Instantiate(gc->bsodaSpray, at, bb::Quat::identity());
  check("prefab-clone-returned", clone != 0, "Instantiate returned null");
  if (!clone) return;

  // BSODA_Spray.prefab is 2 objects / 9 components, so a deep clone must add GameObjects to the
  // registry -- the shallow path added exactly one and attached nothing.
  check("prefab-clone-is-deep", g.gos.size() >= before + 2,
        "registry grew by " + i2s((long long)(g.gos.size() - before)) +
        " (expected >= 2 for BSODA_Spray; 1 means the shallow shell path ran)");
  check("prefab-clone-has-components", !clone->components.empty(),
        "clone root carries no components");
  check("prefab-clone-named-clone", clone->name.size() > 7 &&
        clone->name.compare(clone->name.size() - 7, 7, "(Clone)") == 0,
        "clone root is named '" + clone->name + "' (Unity appends \"(Clone)\")");

  // Instantiate(prefab, position, rotation) places the clone's root there.
  if (clone->transform) {
    bb::Vec3 d = clone->transform->position - at;
    check("prefab-clone-placed-at-position", d.magnitude() < 1e-3f,
          "clone root at (" + f2s(clone->transform->position.x) + "," +
          f2s(clone->transform->position.y) + "," + f2s(clone->transform->position.z) +
          "), requested (" + f2s(at.x) + "," + f2s(at.y) + "," + f2s(at.z) + ")");
  } else {
    fail("prefab-clone-placed-at-position", "clone root has no Transform");
  }

  // Clones are real scene objects: Find must see them (the template must not be findable).
  check("prefab-clone-is-findable", bb::GameObject::Find(clone->name.c_str()) != 0,
        "GameObject::Find could not see the clone '" + clone->name + "'");

  // The specific components BSODA_Spray.prefab authors on its root: a Rigidbody (the spray is
  // physics-pushed), a BsodaSparyScript and a Billboard. GetComponent must resolve all three, and
  // the CapsuleCollider must be registered for physics.
  check("prefab-clone-rigidbody", clone->GetComponent<bb::Rigidbody>() != 0,
        "clone has no Rigidbody");
  check("prefab-clone-script-bound", clone->GetComponent<bb::BsodaSparyScript>() != 0,
        "clone has no BsodaSparyScript");
  // BSODA_Spray.prefab is two objects: the root above, plus a "Sprite" child carrying
  // SpriteRenderer + Billboard. A deep clone has to bring the CHILD across too, so look for the
  // Billboard across the whole newly-added range rather than on the root.
  bool childBillboard = false;
  for (size_t i = before; i < g.gos.size() && !childBillboard; i++) {
    if (g.gos[i]->GetComponent<bb::Billboard>()) childBillboard = true;
  }
  check("prefab-clone-child-billboard", childBillboard,
        "no Billboard anywhere in the cloned subtree (the 'Sprite' child did not come across)");

  // And it must survive being ticked -- this is where unbound fields in prefab scripts surface.
  for (int i = 0; i < 10; i++) eng::runFullFrame(1.0f / 50.0f);
  pass("prefab-clone-survives-frames");

  // AlarmClockDrop (GC item-7): a one-object drop whose scripts need no runtime wiring.
  {
    const size_t pre = g.gos.size();
    bb::GameObject* ac = eng::instantiatePrefab("AlarmClockDrop", bb::Vec3(), bb::Quat::identity());
    if (!ac || g.gos.size() <= pre) {
      fail("prefab-clone-AlarmClockDrop", "instantiatePrefab added no GameObjects");
    } else {
      for (int i = 0; i < 5; i++) eng::runFullFrame(1.0f / 50.0f);
      pass("prefab-clone-AlarmClockDrop");
    }
  }

  // MathGame (NotebookScript's learning game) is the real stress case: 33 objects / 116 components
  // of uGUI, including the TMP_InputField that MathGameScript::playerAnswer derefs in Start.
  //
  // It also pins the DEFERRED-START contract. The prefab serializes `gc` as {fileID:0} because it
  // is a runtime reference, and MathGameScript::Start opens with `gc->ActivateLearningGame()`. So
  // the spawner MUST get a window between Instantiate and Start -- NotebookScript uses exactly
  // that window (`Instantiate(learningGame)` then assigns gc/baldiScript/playerPosition). This
  // test does the same thing; if Start ran synchronously inside Instantiate it would deref null.
  {
    const size_t pre = g.gos.size();
    bb::GameObject* mgRoot = eng::instantiatePrefab("MathGame", bb::Vec3(), bb::Quat::identity());
    if (!mgRoot) {
      fail("prefab-clone-MathGame", "instantiatePrefab returned null (no bake?)");
      fail("prefab-mathgame-inputfield-bound", "no MathGame clone");
      fail("prefab-deferred-start-window", "no MathGame clone");
      return;
    }
    const size_t added = g.gos.size() - pre;
    check("prefab-clone-MathGame", added >= 33,
          "added " + i2s((long long)added) + " GameObjects, expected >= 33");

    bb::MathGameScript* mg = mgRoot->GetComponent<bb::MathGameScript>();
    check("prefab-mathgame-inputfield-bound", mg != 0 && mg->playerAnswer != 0,
          mg ? "MathGameScript::playerAnswer (TMP_InputField) stayed null"
             : "no MathGameScript on the MathGame clone root");
    if (!mg) { fail("prefab-deferred-start-window", "no MathGameScript"); return; }

    // Start must NOT have run yet -- that is the whole point of the window.
    check("prefab-deferred-start-window", mg->gc == 0,
          "MathGameScript::Start already ran inside Instantiate (gc was wired, so a real spawner "
          "would have had no chance to assign it)");

    // Wire it exactly as NotebookScript does, then let the frame run Start.
    mg->gc = gc;
    mg->baldiScript = findAnyIncludingInactive<bb::BaldiScript>();
    // Story mode, first notebook: MathGameScript.NewProblem's normal-question branch is
    // (mode == "story" && (problem <= 2 || notebooks <= 1)), so all three questions are normal.
    if (gc) { gc->mode = "story"; gc->notebooks = 1; }
    for (int i = 0; i < 5; i++) eng::runFullFrame(1.0f / 50.0f);
    pass("prefab-mathgame-starts-after-wiring");

    // ---- A FULL THINKPAD, played by the decompiled rules (MathGameScript.cs) -----------------
    // Answers are typed the way the keypad does it: ButtonPress(0..9) appends a digit,
    // ButtonPress(-1) appends '-', then OKButton() -> CheckAnswer().
    struct Pad {
      static void type(bb::MathGameScript* m, const std::string& ans) {
        m->ButtonPress(-2);                                    // any other value clears the field
        for (size_t i = 0; i < ans.size(); i++) {
          if (ans[i] == '-') m->ButtonPress(-1);
          else if (ans[i] >= '0' && ans[i] <= '9') m->ButtonPress(ans[i] - '0');
        }
        m->OKButton();
      }
    };
    const bool q1Normal = mg->problem == 1 && !mg->impossibleMode;
    Pad::type(mg, bb::csharpToString(mg->solution));
    check("pad-q1-correct-marks-result", q1Normal && mg->results[0] && mg->results[0]->texture == mg->correct &&
          mg->problem == 2,
          "after a correct Q1: problem " + i2s(mg->problem) + ", result1 " +
          (mg->results[0] && mg->results[0]->texture == mg->correct ? "correct" : "not marked correct"));

    Pad::type(mg, "999");                                      // Q2 wrong (a 0..9 +/- answer is never 999)
    for (int i = 0; i < 3; i++) eng::runFullFrame(1.0f / 50.0f);
    // ...and the X's PIXELS must exist: MathGameScript.incorrect (X.png) is referenced only by this
    // field, and the baker used to skip it, so the X never appeared on screen.
    bool xBaked = false;
    if (mg->incorrect) {
      FILE* xf = fopen(("data/tex/" + mg->incorrect->name + ".raw").c_str(), "rb");
      if (xf) { xBaked = true; fclose(xf); }
    }
    check("pad-wrong-answer-x-is-baked", xBaked, "MathGameScript.incorrect has no baked pixels (the X would not draw)");
    check("pad-q2-wrong-marks-result", mg->results[1] && mg->results[1]->texture == mg->incorrect &&
          mg->problemsWrong == 1 && mg->problem == 3,
          "after a wrong Q2: problemsWrong " + i2s(mg->problemsWrong) + ", problem " + i2s(mg->problem));
    check("pad-q2-wrong-starts-spoop", gc && gc->spoopMode,
          "a wrong answer must call gc.ActivateSpoopMode (the chase starts)");
    check("pad-q2-wrong-baldi-frowns", mg->baldiFeed && mg->baldiFeed->stateName == "Baldi_Frown",
          "baldiFeed.SetTrigger(\"angry\") must put the face in Baldi_Frown, got '" +
          (mg->baldiFeed ? mg->baldiFeed->stateName : std::string("no animator")) + "'");

    const bool q3Normal = !mg->impossibleMode;
    Pad::type(mg, bb::csharpToString(mg->solution));
    check("pad-q3-correct-marks-result", q3Normal && mg->results[2] && mg->results[2]->texture == mg->correct &&
          mg->problem == 4,
          "after a correct Q3: problem " + i2s(mg->problem));

    // problem > 3: NewProblem sets endDelay = 5 (MathGameScript.cs:220), counted down on UNSCALED
    // time; then ExitGame -> gc.DeactivateLearningGame destroys the pad. 6 s allows for it.
    bool padGone = false;
    for (int i = 0; i < 300 && !padGone; i++) {
      eng::runFullFrame(1.0f / 50.0f);
      padGone = true;
      for (size_t k = 0; k < g.gos.size(); k++) if (g.gos[k] == mgRoot) { padGone = false; break; }
    }
    check("pad-exits-after-three", padGone, "the ThinkPad was still alive 6 s after the third answer (endDelay is 5 s)");

    // A pad opened DURING spoop mode: MathGameScript.Start moves baldiFeedTransform (a UI
    // RectTransform) to world (-1000,-1000) so Baldi's face is gone. UI layout reads anchored fields,
    // so the world write must reach anchoredPosition (propagateRectWrites) or the face stays.
    if (gc && gc->spoopMode) {
      bb::GameObject* mg2 = eng::instantiatePrefab("MathGame", bb::Vec3(), bb::Quat::identity());
      bb::MathGameScript* m2 = mg2 ? mg2->GetComponent<bb::MathGameScript>() : 0;
      if (m2) {
        m2->gc = gc;
        m2->baldiScript = findAnyIncludingInactive<bb::BaldiScript>();
        bb::RectTransform* fr = dynamic_cast<bb::RectTransform*>(m2->baldiFeedTransform);
        const float before = fr ? fr->anchoredPosition.x : 0.0f;
        for (int i = 0; i < 3; i++) eng::runFullFrame(1.0f / 50.0f);
        const float after = fr ? fr->anchoredPosition.x : 0.0f;
        check("pad-spoop-hides-baldi-face", fr && after < before - 300.0f,
              "baldiFeedTransform anchoredPosition.x " + f2s(before) + " -> " + f2s(after) +
              " (Start's position = (-1000,-1000) must push the face off-screen)");
        // close it the way the game does (ExitGame -> DeactivateLearningGame), or learningActive stays
        // set and GameController keeps Time.timeScale pinned at 0 for every later check
        gc->DeactivateLearningGame(mg2);
        for (int i = 0; i < 3; i++) eng::runFullFrame(1.0f / 50.0f);
      } else {
        fail("pad-spoop-hides-baldi-face", "could not clone a second MathGame");
      }
    }
  }
}

// ---- trigger events (OnTriggerEnter / Stay / Exit) --------------------------------------------
// Built from purpose-made objects rather than driving the real level: the engine synthesises these
// messages by diffing collider overlaps each fixed step, and a synthetic pair pins Enter/Stay/Exit
// exactly once each, in order, without depending on where the player happens to be standing.
struct TriggerProbe : public bb::Component {
  int enters = 0, stays = 0, exits = 0;
  bb::Collider* lastOther = 0;
  void OnTriggerEnter(bb::Collider* o) override { enters++; lastOther = o; }
  void OnTriggerStay(bb::Collider* o) override { stays++; }
  void OnTriggerExit(bb::Collider* o) override { exits++; }
};

// A registered GameObject with a Transform at `p` (the engine needs both for collider world math).
bb::GameObject* makeProbeObject(const char* name, const bb::Vec3& p) {
  bb::GameObject* go = new bb::GameObject();
  go->name = name;
  bb::Transform* t = new bb::Transform();
  t->go = go;
  t->position = p;
  t->rotation = bb::Quat::identity();
  t->scale = bb::Vec3::one();
  go->transform = t;
  eng::noteTransform(t);
  eng::noteGameObject(go);
  return go;
}

// ANIMATOR GRAPH. The real BaldiFeed.controller (decompile: Idle -> Talk on `talking`, Talk ->
// Idle on !`talking`, Any State -> Frown on trigger `angry`) driven with the same calls
// MathGameScript.Update makes, on an Image -- which is what the ThinkPad's Baldi is. Checks the
// STATE and the FRAME actually shown, since the user's report was "his sprite doesn't change".
static bool spriteInClip(const bb::Image* im, const char* clip) {
  eng::State& g = eng::state();
  std::map<std::string, eng::State::SpriteAnimKeys>::const_iterator it = g.spriteAnims.find(clip);
  if (!im || !im->sprite || it == g.spriteAnims.end()) return false;
  for (size_t i = 0; i < it->second.keys.size(); i++)
    if (it->second.keys[i].second == im->sprite->name) return true;
  return false;
}

void testAnimatorGraph() {
  bb::GameObject* go = makeProbeObject("ProbeBaldiFeed", bb::Vec3(0.0f, 0.0f, 0.0f));
  bb::Image* im = go->AddComponent<bb::Image>();
  bb::Animator* an = go->AddComponent<bb::Animator>();
  an->controllerGuid = "ae0da807ad41ac34da658ac3c8fa6925";   // BaldiFeed.controller
  for (int f = 0; f < 3; f++) an->Update(0.02f);
  check("anim-baldifeed-starts-idle", an->stateName == "Baldi_Idle" && spriteInClip(im, "Baldi_Idle"),
        "state '" + an->stateName + "', frame " + (im->sprite ? im->sprite->name : std::string("none")));
  an->SetBool("talking", true);                               // MathGameScript: baldiAudio playing
  for (int f = 0; f < 3; f++) an->Update(0.02f);
  check("anim-baldifeed-talks", an->stateName == "Baldi_Talk" && spriteInClip(im, "Baldi_Talk"),
        "after SetBool(talking,true): state '" + an->stateName + "'");
  an->SetBool("talking", false);
  for (int f = 0; f < 3; f++) an->Update(0.02f);
  check("anim-baldifeed-stops-talking", an->stateName == "Baldi_Idle" && spriteInClip(im, "Baldi_Idle"),
        "after SetBool(talking,false): state '" + an->stateName + "'");
  an->SetTrigger("angry");                                    // MathGameScript: wrong answer
  an->Update(0.02f);
  const bool frown = an->stateName == "Baldi_Frown" && spriteInClip(im, "Baldi_Frown");
  for (int f = 0; f < 3; f++) an->Update(0.02f);
  check("anim-baldifeed-angry-frowns-once", frown && an->stateName == "Baldi_Frown" &&
        eng::state().animTriggers[an].count("angry") == 0,
        "after SetTrigger(angry): state '" + an->stateName + "' (trigger must be consumed)");
  // UPDATE MODE: the ThinkPad runs at Time.timeScale 0. BaldiFeed is UnscaledTime (2) and must
  // keep cycling its Talk frames; a Normal (0) animator must hold still, as Unity's does.
  {
    const float savedScale = bb::Time::timeScale;
    bb::Time::timeScale = 0.0f;
    an->SetBool("talking", true);
    an->Update(0.0f);                                          // enter Talk
    an->updateMode = 2;
    eng::state().dtUnscaled = 0.02f;
    const float t0 = an->clipTime;
    for (int f = 0; f < 5; f++) an->Update(0.0f);              // scaled dt is 0 at timeScale 0
    const float advUnscaled = an->clipTime - t0;
    an->updateMode = 0;
    const float t1 = an->clipTime;
    for (int f = 0; f < 5; f++) an->Update(0.0f);
    const float advNormal = an->clipTime - t1;
    bb::Time::timeScale = savedScale;
    // (the probe is in Baldi_Frown here -- a dead end in BaldiFeed.controller, speed 0.5 -- so 5
    // unscaled steps of 0.02 advance 0.05 s of clip time)
    const float expect = 5.0f * 0.02f * an->clipSpeed;
    check("anim-unscaled-runs-at-timescale-0", fabsf(advUnscaled - expect) < 1e-4f && advNormal == 0.0f,
          "UnscaledTime advanced " + f2s(advUnscaled) + " s, Normal advanced " + f2s(advNormal) + " s");
  }
  eng::destroyGameObjectNow(go);

  // Baldi's slap (BaldiScript: baldiAnimator.SetTrigger("slap")) on the SpriteRenderer path.
  bb::GameObject* bgo = makeProbeObject("ProbeBaldiSlap", bb::Vec3(0.0f, 0.0f, 0.0f));
  bb::SpriteRenderer* sr = bgo->AddComponent<bb::SpriteRenderer>();
  bb::Animator* ban = bgo->AddComponent<bb::Animator>();
  ban->controllerGuid = "3af339b8278ffa7488ca9518b51565c0";  // Baldi slap controller
  for (int f = 0; f < 30; f++) ban->Update(0.02f);            // let the default clip run out
  const std::string before = sr->sprite ? sr->sprite->name : std::string();
  ban->SetTrigger("slap");
  ban->Update(0.02f);
  check("anim-baldi-slap-retriggers", ban->stateName == "Baldi_Slap" && ban->clipTime < 0.05f &&
        eng::state().animTriggers[ban].count("slap") == 0,
        "slap trigger did not restart the clip (state '" + ban->stateName + "')");
  (void)before;
  eng::destroyGameObjectNow(bgo);
}

// VOICE EXPIRY ON THE MIX CLOCK (user: "Baldi is cut off when he's about to finish a sentence").
// MathGameScript plays the next queued clip the instant baldiAudio.isPlaying goes false. With the
// mixer live, a one-shot must stay "playing" until the MIXER has rendered its last sample -- even if
// game time has already run past the clip's length because the audio device lags.
void testVoiceExpiry() {
  bb::AudioClip* clip = eng::clipForGuid("b31ebb0fc77ac05448b9e76637d4bbcb");   // BAL_Math_Equals.wav
  if (!clip) { fail("audio-voice-ends-on-mix-clock", "BAL_Math_Equals.wav not registered"); return; }
  clip->loadPcm();
  if (!clip->pcmLoaded || clip->rate <= 0 || clip->channels <= 0) {
    fail("audio-voice-ends-on-mix-clock", "clip PCM did not load");
    return;
  }
  const double lenSec = (double)(clip->pcm.size() / (size_t)clip->channels) / (double)clip->rate;
  bb::GameObject* go = makeProbeObject("ProbeVoice", bb::Vec3(0.0f, 0.0f, 0.0f));
  bb::AudioSource* src = go->AddComponent<bb::AudioSource>();
  std::vector<int16_t> buf(4410 * 2);
  bb::audioPull(&buf[0], 4410);                                  // mixer is live from here on
  src->PlayOneShot(clip, 1.0f);
  // game time runs to 1.5x the clip while the device has only pulled half of it
  const int frames = (int)(lenSec * 1.5 / 0.02) + 1;
  const int pullTotal = (int)(lenSec * 0.5 * 44100.0);
  int pulled = 0;
  for (int f = 0; f < frames; f++) {
    const int want = (pullTotal - pulled) < 441 ? (pullTotal - pulled) : 441;
    if (want > 0) { bb::audioPull(&buf[0], want); pulled += want; }
    eng::runFullFrame(0.02f);
  }
  const bool stillPlayingMidTail = src->isPlaying();
  // the device catches up: pull the rest of the clip plus a little
  const int rest = (int)(lenSec * 44100.0) - pulled + 2205;
  for (int left = rest; left > 0; left -= 4410) bb::audioPull(&buf[0], left < 4410 ? left : 4410);
  eng::runFullFrame(0.02f);
  check("audio-voice-ends-on-mix-clock", stillPlayingMidTail && !src->isPlaying(),
        std::string("after 1.5x the clip in game time but only half of it mixed, isPlaying was ") +
        (stillPlayingMidTail ? "true" : "FALSE (the tail would be cut)") + "; after the mixer passed its end: " +
        (src->isPlaying() ? "still true" : "false"));
  // REAL VOICE LIMIT (AudioManager.asset m_RealVoiceCount 32): past 32 voices Unity virtualises
  // the rest. 40 SEPARATE sources (one copy of a clip per source is all that sounds -- see
  // shotIsDuplicate) each start the clip at the same sample; quiet enough not to clip, 32 and 40
  // voices must mix to the SAME output if the cap holds (40 would be 25% louder).
  {
    std::vector<bb::AudioSource*> savedSources = eng::state().audioSources;
    std::vector<bb::AudioSource*> many;
    for (int k = 0; k < 40; k++) {
      bb::GameObject* o = makeProbeObject("ProbeVoice", src->transform ? src->transform->position : bb::Vec3());
      bb::AudioSource* a = o->AddComponent<bb::AudioSource>();
      a->volume = 0.002f;
      a->spatial = 0.0f;                       // 2D: no distance roll-off in the comparison
      many.push_back(a);
    }
    std::vector<int16_t> a32(2205 * 2), a40(2205 * 2);
    eng::state().audioSources.assign(many.begin(), many.begin() + 32);
    for (int k = 0; k < 32; k++) many[k]->PlayOneShot(clip, 1.0f);
    bb::audioPull(&a32[0], 2205);
    for (int k = 0; k < 32; k++) many[k]->Stop();
    eng::state().audioSources.assign(many.begin(), many.end());
    for (int k = 0; k < 40; k++) many[k]->PlayOneShot(clip, 1.0f);
    bb::audioPull(&a40[0], 2205);
    for (int k = 0; k < 40; k++) many[k]->Stop();
    long long e32 = 0, e40 = 0;
    for (size_t k = 0; k < a32.size(); k++) { e32 += a32[k] < 0 ? -a32[k] : a32[k]; e40 += a40[k] < 0 ? -a40[k] : a40[k]; }
    eng::state().audioSources = savedSources;
    for (int k = 0; k < 40; k++) many[k]->gameObject->SetActive(false);
    check("audio-real-voice-cap-32", e32 > 0 && e32 == e40,
          "32 voices mixed to |sum| " + i2s(e32) + ", 40 voices to " + i2s(e40) + " (must match: Unity plays 32)");
  }
  // ONE COPY PER CLIP PER SOURCE: 20 one-shots of the same clip on ONE source sound exactly like
  // one (the game-over's per-frame screech == the single BAL_Screech, per the genuine game).
  {
    std::vector<bb::AudioSource*> savedSources = eng::state().audioSources;
    bb::GameObject* o = makeProbeObject("ProbeOneCopy", src->transform ? src->transform->position : bb::Vec3());
    bb::AudioSource* a = o->AddComponent<bb::AudioSource>();
    a->volume = 0.05f;
    a->spatial = 0.0f;
    eng::state().audioSources.assign(1, a);
    std::vector<int16_t> one(2205 * 2), twenty(2205 * 2);
    a->PlayOneShot(clip, 1.0f);
    bb::audioPull(&one[0], 2205);
    a->Stop();
    for (int k = 0; k < 20; k++) a->PlayOneShot(clip, 1.0f);
    bb::audioPull(&twenty[0], 2205);
    a->Stop();
    long long e1 = 0, e20 = 0;
    for (size_t k = 0; k < one.size(); k++) { e1 += one[k] < 0 ? -one[k] : one[k]; e20 += twenty[k] < 0 ? -twenty[k] : twenty[k]; }
    eng::state().audioSources = savedSources;
    o->SetActive(false);
    check("audio-one-copy-per-clip-per-source", e1 > 0 && e1 == e20,
          "one one-shot mixed to |sum| " + i2s(e1) + ", 20 of the same clip on the same source to " + i2s(e20) + " (must match)");
  }
  eng::destroyGameObjectNow(go);
}


// 1st Prize pinning the player against a wall (user report: the pin never ends). Genuine rules
// (PlayerScript.cs / FirstPrizeScript.cs): hugging is set by OnTriggerStay only while 1st Prize's
// agent velocity > 5 and lapses 1 s later (sweepingFailsave); 1st Prize keeps re-targeting the
// player. Logs the pin over time so the failure mode is visible, then checks it ends.
void testFirstPrizePin() {
  eng::unloadScene(true);
  eng::loadScene("School");
  for (int f = 0; f < 5; f++) eng::runFullFrame(1.0f / 50.0f);
  eng::State& g = eng::state();
  for (size_t i = 0; i < g.gos.size(); i++)      // the NPCs (1st Prize too) wake with spoop mode
    if (bb::GameControllerScript* gc = g.gos[i]->GetComponent<bb::GameControllerScript>()) { gc->ActivateSpoopMode(); break; }
  for (int f = 0; f < 5; f++) eng::runFullFrame(1.0f / 50.0f);
  // only 1st Prize stays awake: Baldi would end the game, the others drag the player elsewhere
  const char* others[] = { "Baldi", "Principal of the Thing", "Playtime", "Gotta Sweep", "Its a Bully", "Arts and Crafters" };
  for (size_t i = 0; i < g.gos.size(); i++)
    for (size_t k = 0; k < sizeof(others) / sizeof(others[0]); k++)
      if (g.gos[i]->name == others[k]) g.gos[i]->SetActive(false);
  bb::PlayerScript* ps = 0;
  bb::FirstPrizeScript* fp = 0;
  for (size_t i = 0; i < g.gos.size(); i++)
    for (size_t k = 0; k < g.gos[i]->components.size(); k++) {
      if (bb::PlayerScript* x = dynamic_cast<bb::PlayerScript*>(g.gos[i]->components[k])) ps = x;
      if (bb::FirstPrizeScript* x = dynamic_cast<bb::FirstPrizeScript*>(g.gos[i]->components[k])) fp = x;
    }
  if (!ps || !fp || !fp->agent) { fail("first-prize-pin-ends", "no PlayerScript / FirstPrizeScript / agent"); return; }
  bb::Transform* pt = ps->transform;
  // a hallway spot: a "Wall" panel 25..60 units ahead along an axis, with the hall clear 10 units
  // behind (scan the School's 10-unit grid at eye height)
  bb::Vec3 dir, from;
  bb::RaycastHit hit;
  bool found = false;
  const bb::Vec3 axes[4] = { bb::Vec3(1, 0, 0), bb::Vec3(-1, 0, 0), bb::Vec3(0, 0, 1), bb::Vec3(0, 0, -1) };
  for (int gx = 0; gx < 20 && !found; gx++)
    for (int gz = 0; gz < 20 && !found; gz++)
      for (int a = 0; a < 4 && !found; a++) {
        const bb::Vec3 o(5.0f + gx * 10.0f, 4.0f, 5.0f + gz * 10.0f);
        bb::RaycastHit h, back;
        if (bb::Physics::Raycast(o, axes[a], &h, 200.0f, ~0, bb::QueryTriggerInteraction::Ignore) &&
            h.gameObject && h.gameObject->name == "Wall" && h.distance > 25.0f && h.distance < 60.0f &&
            !(bb::Physics::Raycast(o, axes[a] * -1.0f, &back, 10.0f, ~0, bb::QueryTriggerInteraction::Ignore))) {
          dir = axes[a]; hit = h; from = o; found = true;
        }
      }
  if (!found) { fail("first-prize-pin-ends", "no hallway wall found on the grid"); return; }
  if (getenv("SIM_PRIZE")) printf("PRIZE hall from (%.0f,%.0f) dir (%.0f,%.0f) wall %.1f u\n", from.x, from.z, dir.x, dir.z, hit.distance);
  const bb::Vec3 wall(hit.point.x, pt->position.y, hit.point.z);
  const bb::Vec3 side(dir.z, 0.0f, -dir.x);       // across the 10-wide hall (centre line = from)
  // (player along-hall distance from the end wall, player across, prize behind, prize across)
  struct Sc { const char* name; float pAlong, pSide, fBack, fSide; };
  const Sc sc[] = {
    { "head-on into the end wall",       1.2f,  0.0f, 14.0f,  0.0f },
    { "pushed down the hall",           15.0f,  0.0f, 12.0f,  0.0f },
    { "against a side wall, diagonal",  12.0f,  3.8f, 12.0f, -2.0f },
    { "into the corner",                 1.2f,  3.8f, 14.0f, -2.0f },
    { "across the hall",                12.0f,  3.8f,  0.5f, -3.5f },
  };
  float worst = 0.0f;
  std::string worstName;
  const bool verbose = getenv("SIM_PRIZE") != 0;
  for (size_t k = 0; k < sizeof(sc) / sizeof(sc[0]); k++) {
    pt->position = wall - dir * sc[k].pAlong + side * sc[k].pSide;
    ps->hugging = false;
    const bb::Vec3 fpAt = wall - dir * (sc[k].pAlong + sc[k].fBack) + side * sc[k].fSide;
    fp->agent->Warp(bb::Vec3(fpAt.x, 0.0f, fpAt.z));
    const bb::Vec3 toP = bb::normalized(bb::Vec3(pt->position.x - fpAt.x, 0.0f, pt->position.z - fpAt.z));
    const float yaw = atan2f(toP.x, toP.z);
    fp->transform->rotation = bb::Quat(0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f));
    float hugSince = -1.0f, longest = 0.0f;
    bool everHugged = false;
    for (int f = 0; f < 50 * 30; f++) {   // 30 s, no input
      eng::runFullFrame(1.0f / 50.0f);
      const float t = f / 50.0f;
      if (ps->hugging) { everHugged = true; if (hugSince < 0.0f) hugSince = t; longest = (std::max)(longest, t - hugSince); }
      else hugSince = -1.0f;
      if (verbose && f % 50 == 0) {
        const bb::Vec3 fv = fp->agent->velocity();
        bb::RaycastHit rh;
        const bool rs = bb::Physics::Raycast(fp->transform->position, pt->position - fp->transform->position, &rh,
                                             std::numeric_limits<float>::infinity(), 769, bb::QueryTriggerInteraction::Ignore);
        printf("PRIZE [%s] t=%4.0f ts=%.1f seen=%d ray=%s yaw=%.1f stopped=%d act=%d en=%d hug=%d fpVel=%6.2f fp=(%.2f,%.2f,%.2f) pl=(%.2f,%.2f) speed=%.0f ang=%.1f%s",
               sc[k].name, t, bb::Time::timeScale, (int)fp->playerSeen, rs && rh.gameObject ? rh.gameObject->name.c_str() : "-",
               fp->transform->eulerAngles().y, (int)fp->agent->isStopped, (int)fp->gameObject->activeInHierarchy(), (int)fp->enabled,
               (int)ps->hugging, fv.magnitude(), fp->transform->position.x, fp->transform->position.y, fp->transform->position.z,
               pt->position.x, pt->position.z, fp->agent->speed, fp->angleDiff, "\n");
      }
    }
    printf("PRIZE [%s] hugged %s, longest pin %.1f s%s", sc[k].name, everHugged ? "yes" : "no", longest, "\n");
    if (longest > worst) { worst = longest; worstName = sc[k].name; }
  }
  // ESCAPE (user: "i escape and he immediately hugs again"). Pin the player head-on against the
  // end wall, wait for the hug to lapse, then strafe away along the wall (D held) and time how long
  // it takes him to hug again. Genuine: hugging needs agent velocity > 5, and the agent starts
  // from its ACTUAL standstill at acceleration 10 -> about half a second before it can re-hug.
  {
    pt->position = wall - dir * 1.2f;
    ps->hugging = false;
    const bb::Vec3 fpAt = wall - dir * 15.2f;
    fp->agent->Warp(bb::Vec3(fpAt.x, 0.0f, fpAt.z));
    const float yaw = atan2f(dir.x, dir.z);
    fp->transform->rotation = bb::Quat(0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f));
    ps->transform->rotation = bb::Quat(0.0f, sinf((yaw + 3.14159265f) * 0.5f), 0.0f, cosf((yaw + 3.14159265f) * 0.5f));
    ps->playerRotation = ps->transform->rotation;   // facing 1st Prize, back to the wall
    bool pinned = false, released = false;
    int f = 0;
    for (; f < 50 * 10 && !released; f++) {
      eng::runFullFrame(1.0f / 50.0f);
      if (ps->hugging) pinned = true;
      else if (pinned) released = true;
    }
    float rehug = -1.0f;
    const bb::Vec3 start = pt->position;
    float away = 0.0f;
    if (released) {
      eng::injectKey((int)bb::KeyCode::D, true);
      for (int k = 0; k < 50 * 3; k++) {
        eng::runFullFrame(1.0f / 50.0f);
        if (verbose && k % 5 == 0)
          printf("PRIZE [escape] t=%.2f hug=%d fpVel=%.2f fpSpeed=%.0f ang=%.1f gap=%.2f\n", k / 50.0f, (int)ps->hugging,
                 fp->agent->velocity().magnitude(), fp->agent->speed, fp->angleDiff,
                 bb::length(bb::Vec3(pt->position.x - fp->transform->position.x, 0.0f, pt->position.z - fp->transform->position.z)));
        if (ps->hugging) { rehug = k / 50.0f; away = bb::length(pt->position - start); break; }
      }
      eng::injectKeyRelease((int)bb::KeyCode::D);
      eng::runFullFrame(1.0f / 50.0f);
    }
    if (verbose) printf("PRIZE [escape] pinned %d released %d after %.1f s, re-hug %.2f s (moved %.1f u)\n",
                        (int)pinned, (int)released, f / 50.0f, rehug, away);
    check("first-prize-escape-window", released && (rehug < 0.0f || rehug >= 0.4f),
          std::string(released ? "released" : "NEVER released") + "; strafing away he hugged again after " +
          (rehug < 0.0f ? std::string("never (3 s)") : f2s(rehug) + " s") + " (genuine: >= ~0.5 s from a standstill at acceleration 10)");
  }
  check("first-prize-pin-ends", worst < 10.0f,
        "longest continuous pin " + f2s(worst) + " s (" + worstName + "), must be < 10 s");
}


// User reports (session 11r), each pressed through the real School objects:
//   yellow doors played their opening sound twice; the Principal stood sunk in the floor at
//   detention; BSODA hung in the air and pushed nobody; quarters did nothing at the machines/phone.
static void faceFrom(bb::PlayerScript* ps, const bb::Vec3& at, const bb::Vec3& target) {
  ps->transform->position = at;
  const float yaw = atan2f(target.x - at.x, target.z - at.z);
  ps->transform->rotation = bb::Quat(0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f));
  ps->playerRotation = ps->transform->rotation;
}

// A spot 5 units off `name` (one of the four axes) whose ray to it hits it first, at eye height.
static bool aimSpot(const char* name, float eye, bb::Vec3& at, bb::Vec3& tgt) {
  eng::State& g = eng::state();
  for (size_t i = 0; i < g.gos.size(); i++) {
    if (g.gos[i]->name != name || !g.gos[i]->activeInHierarchy()) continue;
    const bb::Vec3 m = g.gos[i]->transform->position;
    const bb::Vec3 dirs[4] = { bb::Vec3(1, 0, 0), bb::Vec3(-1, 0, 0), bb::Vec3(0, 0, 1), bb::Vec3(0, 0, -1) };
    for (int d = 0; d < 4; d++) {
      const bb::Vec3 p(m.x + dirs[d].x * 5.0f, eye, m.z + dirs[d].z * 5.0f);
      const bb::Vec3 aimAt(m.x, eye, m.z);
      bb::RaycastHit h;
      if (bb::Physics::Raycast(p, aimAt - p, &h, 20.0f, ~0) && h.gameObject == g.gos[i]) { at = p; tgt = aimAt; return true; }
    }
  }
  return false;
}

void testUserReport11r() {
  eng::unloadScene(true);
  eng::loadScene("School");
  for (int f = 0; f < 5; f++) eng::runFullFrame(1.0f / 50.0f);
  eng::State& g = eng::state();
  bb::PlayerScript* ps = 0;
  bb::GameControllerScript* gc = 0;
  bb::PrincipalScript* pr = 0;
  bb::SwingingDoorScript* door = 0;
  for (size_t i = 0; i < g.gos.size(); i++)
    for (size_t k = 0; k < g.gos[i]->components.size(); k++) {
      bb::Component* c = g.gos[i]->components[k];
      if (bb::PlayerScript* x = dynamic_cast<bb::PlayerScript*>(c)) ps = x;
      if (bb::GameControllerScript* x = dynamic_cast<bb::GameControllerScript*>(c)) gc = x;
      if (bb::PrincipalScript* x = dynamic_cast<bb::PrincipalScript*>(c)) pr = x;
      if (bb::SwingingDoorScript* x = dynamic_cast<bb::SwingingDoorScript*>(c)) { if (!door) door = x; }
    }
  if (!ps || !gc || !pr || !door) { fail("report-11r-setup", "PlayerScript / GC / Principal / SwingingDoor missing"); return; }
  const bb::Vec3 home = ps->transform->position;
  const float eyeY = home.y;
  bb::NavMeshAgent* prAgent = pr->gameObject->GetComponent<bb::NavMeshAgent>();   // Start (and agent) may not have run yet
  if (!prAgent) { fail("report-11r-setup", "Principal has no NavMeshAgent"); return; }

  // 1. swinging door: one walk through = one OnTriggerEnter = one doorOpen PlayOneShot
  {
    gc->notebooks = 2;                                        // unlocks the hall doors
    for (int f = 0; f < 3; f++) eng::runFullFrame(1.0f / 50.0f);
    const bb::Vec3 c = door->transform->position;
    const bb::Vec3 n = bb::normalized(door->transform->rotation.rotate(bb::Vec3(0.0f, 0.0f, 1.0f)));
    size_t most = 0;
    for (int step = -12; step <= 12; step++) {
      const bb::Vec3 at(c.x + n.x * step * 0.5f, eyeY, c.z + n.z * step * 0.5f);
      faceFrom(ps, at, bb::Vec3(at.x + n.x, eyeY, at.z + n.z));
      eng::runFullFrame(1.0f / 50.0f);
      const size_t shots = door->myAudio ? g.audioSim[door->myAudio].shots.size() : 0;
      most = (std::max)(most, shots);
    }
    check("swinging-door-sound-once", !door->bDoorLocked && most == 1,
          std::string("door ") + (door->bDoorLocked ? "LOCKED" : "unlocked") + ", doorOpen voices at once while walking through: " +
          i2s((long long)most) + " (1: the player's static trigger capsule must not message a static trigger)");
    gc->notebooks = 0;
  }

  // 2. the Principal at detention stands at his walking height (navmesh + baseOffset 1 x scale 2)
  {
    const float standY = pr->transform->position.y;
    prAgent->Warp(pr->principalDetentionPoint->position);
    const float warpedY = pr->transform->position.y;
    check("principal-detention-warp-height", fabsf(warpedY - standY) < 0.05f && fabsf(standY - 2.017f) < 0.05f,
          "after Warp(Principal_DetentionWarpPoint, y 0) he stands at y " + f2s(warpedY) + ", his authored height is " + f2s(standY));
  }

  // 3. BSODA: the spray flies forward at speed 20 and carries an NPC in its path
  {
    faceFrom(ps, home, bb::Vec3(home.x, eyeY, home.z + 10.0f));
    for (int f = 0; f < 3; f++) eng::runFullFrame(1.0f / 50.0f);
    bb::GameObject* target = pr->gameObject;
    target->SetActive(true);
    pr->enabled = false;                                      // only the BSODA moves him
    const bb::Vec3 fwd = bb::normalized(bb::Vec3(ps->transform->forward().x, 0.0f, ps->transform->forward().z));
    prAgent->Warp(home + fwd * 8.0f);
    prAgent->isStopped = false;
    prAgent->SetDestination(pr->transform->position);
    const bb::Vec3 npcStart = pr->transform->position;
    gc->item[0] = 4;
    gc->itemSelected = 0;
    gc->UseItem();
    bb::BsodaSparyScript* spray = 0;
    eng::runFullFrame(1.0f / 50.0f);
    for (size_t i = 0; i < g.gos.size() && !spray; i++) spray = g.gos[i]->GetComponent<bb::BsodaSparyScript>();
    const bb::Vec3 s0 = spray ? spray->transform->position : bb::Vec3();
    for (int f = 0; f < 25; f++) eng::runFullFrame(1.0f / 50.0f);
    const bb::Vec3 s1 = spray ? spray->transform->position : bb::Vec3();
    const float flown = bb::dot(s1 - s0, fwd);
    for (int f = 0; f < 25; f++) eng::runFullFrame(1.0f / 50.0f);
    const float pushed = bb::dot(pr->transform->position - npcStart, fwd);
    check("bsoda-spray-flies", spray && fabsf(flown - 10.0f) < 1.0f,
          std::string("spray ") + (spray ? "spawned" : "MISSING") + ", flew " + f2s(flown) + " units forward in 0.5 s (speed 20 -> 10)");
    check("bsoda-pushes-npc", pushed > 3.0f,
          "NPC in the spray's path moved " + f2s(pushed) + " units along it (BsodaEffectScript: agent.velocity = spray velocity)");
    pr->enabled = true;
    target->SetActive(false);
  }

  // 4. quarter (item 5) at a BSODA machine -> a BSODA; at the payphone -> the tape plays
  {
    bb::Vec3 at, tgt;
    int got = -1;
    // UseItem casts from the CAMERA (ScreenPointToRay at the screen centre): aim from its height
    const float camY = bb::Camera::main() && bb::Camera::main()->transform ? bb::Camera::main()->transform->position.y : eyeY;
    if (aimSpot("BSODAMachine", camY, at, tgt)) {
      faceFrom(ps, bb::Vec3(at.x, eyeY, at.z), bb::Vec3(tgt.x, eyeY, tgt.z));
      for (int f = 0; f < 3; f++) eng::runFullFrame(1.0f / 50.0f);
      gc->item[0] = 5; gc->item[1] = 0; gc->item[2] = 0;
      gc->itemSelected = 0;
      gc->UseItem();
      got = gc->item[0];
    }
    check("quarter-buys-bsoda", got == 4, "quarter at a BSODAMachine left slot 1 holding item " + i2s(got) + " (4 = BSODA)");
    bb::TapePlayerScript* tape = 0;
    bool phone = false;
    // Baldi awake (spoop mode), as in real play: TapePlayer.Play activates his anti-hearing, and
    // TapePlayerScript.Update then calls UnPause every frame -- the path that silenced the phone.
    gc->ActivateSpoopMode();
    for (int f = 0; f < 2; f++) eng::runFullFrame(1.0f / 50.0f);
    if (aimSpot("PayPhone", camY, at, tgt)) {
      faceFrom(ps, bb::Vec3(at.x, eyeY, at.z), bb::Vec3(tgt.x, eyeY, tgt.z));
      for (int f = 0; f < 3; f++) eng::runFullFrame(1.0f / 50.0f);
      gc->item[0] = 5;
      gc->itemSelected = 0;
      gc->UseItem();
      for (size_t i = 0; i < g.gos.size(); i++)
        if (g.gos[i]->name == "PayPhone") tape = g.gos[i]->GetComponent<bb::TapePlayerScript>();
      phone = tape && tape->audioDevice && tape->audioDevice->isPlaying() && gc->item[0] == 0;
    }
    // ...and the tape must be AUDIBLE through the real mixer (user: "the 25 cent phone doesnt
    // make a sound"): mute everything else, then pull the mix.
    float loudest = 0.0f;
    if (phone) {
      eng::sceneShown();
      for (size_t i = 0; i < g.audioSources.size(); i++)
        if (g.audioSources[i] != tape->audioDevice) g.audioSources[i]->mute = true;
      short pb[1024 * 2];
      for (int k = 0; k < 20; k++) {
        eng::runFullFrame(1.0f / 50.0f);
        bb::audioPull(pb, 1024);
        for (int i = 0; i < 1024 * 2; i++) loudest = (std::max)(loudest, (float)abs(pb[i]));
      }
      for (size_t i = 0; i < g.audioSources.size(); i++) g.audioSources[i]->mute = false;
    }
    check("quarter-payphone-audible", phone && loudest > 300.0f,
          "PayPhone tape peak " + f2s(loudest) + " (int16) through the mixer with every other source muted");
    check("quarter-rings-payphone", phone, std::string("PayPhone ") + (tape ? "found" : "not aimed at") +
          ", tape " + (tape && tape->audioDevice && tape->audioDevice->isPlaying() ? "playing" : "SILENT") + ", quarter " +
          (gc->item[0] == 0 ? "spent" : "still held"));
  }
  ps->transform->position = home;
}


// Diagnostic (SIM_GAMEOVER_WAV=<path>): the game-over jumpscare through the REAL mixer. Sets
// player.gameOver in the School, runs the 1 s delay at 50 fps pulling 882 samples per frame (the
// shell's deterministic --wav-out cadence), writes the stereo int16 mix to <path>.
static void dumpGameOverWav(const char* path) {
  eng::unloadScene(true);
  eng::loadScene("School");
  for (int f = 0; f < 5; f++) eng::runFullFrame(1.0f / 50.0f);
  eng::State& g = eng::state();
  bb::PlayerScript* ps = 0;
  for (size_t i = 0; i < g.gos.size() && !ps; i++) ps = g.gos[i]->GetComponent<bb::PlayerScript>();
  if (!ps) return;
  eng::sceneShown();   // no frame is presented in the sim: release the scene-swap audio hold
  // 60 fps (the user's 59.94 Hz display; one screech copy per frame), the FLOAT mix the Windows
  // build outputs, scaled x0.1 into int16 so the file holds the unclamped shape.
  std::vector<short> cap;
  float buf[735 * 2];
  for (int f = 0; f < 10; f++) { eng::runFullFrame(1.0f / 60.0f); bb::audioPullFloat(buf, 735); }   // settle
  cap.clear();
  ps->gameOver = true;
  for (int f = 0; f < 60 * 3 && eng::currentSceneName() == "School"; f++) {
    eng::runFullFrame(1.0f / 60.0f);
    bb::audioPullFloat(buf, 735);
    for (int i = 0; i < 735 * 2; i++) {
      const float v = buf[i] * 0.1f * 32767.0f;
      cap.push_back((short)(v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v));
    }
  }
  FILE* wf = fopen(path, "wb");
  if (!wf) return;
  const unsigned int dataBytes = (unsigned int)(cap.size() * sizeof(short));
  unsigned char h[44];
  memcpy(h + 0, "RIFF", 4); *(unsigned int*)(h + 4) = 36 + dataBytes;
  memcpy(h + 8, "WAVEfmt ", 8); *(unsigned int*)(h + 16) = 16;
  *(unsigned short*)(h + 20) = 1; *(unsigned short*)(h + 22) = 2;
  *(unsigned int*)(h + 24) = 44100; *(unsigned int*)(h + 28) = 44100 * 4;
  *(unsigned short*)(h + 32) = 4; *(unsigned short*)(h + 34) = 16;
  memcpy(h + 36, "data", 4); *(unsigned int*)(h + 40) = dataBytes;
  fwrite(h, 1, 44, wf);
  if (!cap.empty()) fwrite(&cap[0], 1, dataBytes, wf);
  fclose(wf);
  printf("GAMEOVER WAV %s: %u frames\n", path, (unsigned)(cap.size() / 2));
}


// LoadSceneAsync (StartButton) must not let the School's play-on-awake audio run while the swap
// is still loading: the mixer is silent and its clock frozen until the new scene's first frame is
// presented, and the held voices then start from their first sample.
void testSceneSwapAudioHold() {
  eng::unloadScene(true);
  eng::loadScene("MainMenu");
  for (int f = 0; f < 3; f++) eng::runFullFrame(1.0f / 50.0f);
  eng::State& g = eng::state();
  short buf[882 * 2];
  bb::audioPull(buf, 882);                       // a live mixer (mixClock > 0)
  bb::LoadSceneAsync("School");
  for (int f = 0; f < 4 && eng::currentSceneName() != "School"; f++) eng::runFullFrame(1.0f / 50.0f);
  const bool swapped = eng::currentSceneName() == "School";
  const bool held = g.audioHold.load();
  const long long clk0 = g.mixClock.load();
  bool silent = true;
  for (int k = 0; k < 20; k++) {                 // ~0.4 s of output while "still loading"
    bb::audioPull(buf, 882);
    for (int i = 0; i < 882 * 2; i++) if (buf[i] != 0) silent = false;
  }
  const bool frozen = g.mixClock.load() == clk0;
  eng::sceneShown();
  bool sound = false;
  for (int k = 0; k < 20 && !sound; k++) {
    bb::audioPull(buf, 882);
    for (int i = 0; i < 882 * 2; i++) if (buf[i] != 0) sound = true;
  }
  check("scene-swap-audio-held-until-shown", swapped && held && silent && frozen && sound && !g.audioHold.load(),
        std::string("async swap ") + (swapped ? "done" : "NOT done") + "; hold " + (held ? "on" : "OFF") +
        ", output while loading " + (silent ? "silent" : "AUDIBLE") + ", clock " + (frozen ? "frozen" : "RAN") +
        "; after the first frame: " + (sound ? "School audio plays" : "SILENT"));
}


// The jumpscare camera after a REAL chase (user recording: the port showed a close-up brick wall
// where the genuine game shows Baldi's face). Baldi starts down a hall, hunts the player with his
// own AI (Hear at the player + his slap movement), and catches through the real trigger. Then:
// CameraScript puts the camera at baldi + baldi.forward * 2 + (0,5,0) looking at Baldi -- a ray
// from the camera to Baldi's head must reach Baldi, not a wall.
void testJumpscareCamera() {
  const float angles[4] = { 0.0f, 90.0f, 180.0f, 270.0f };
  int seen = 0, caught = 0;
  std::string detail;
  for (int a = 0; a < 4; a++) {
    eng::unloadScene(true);
    eng::loadScene("School");
    for (int f = 0; f < 5; f++) eng::runFullFrame(1.0f / 50.0f);
    eng::State& g = eng::state();
    bb::PlayerScript* ps = 0;
    bb::GameControllerScript* gc = 0;
    bb::BaldiScript* bs = 0;
    bb::CameraScript* cs = 0;
    for (size_t i = 0; i < g.gos.size(); i++)
      for (size_t k = 0; k < g.gos[i]->components.size(); k++) {
        bb::Component* c = g.gos[i]->components[k];
        if (bb::PlayerScript* x = dynamic_cast<bb::PlayerScript*>(c)) ps = x;
        if (bb::GameControllerScript* x = dynamic_cast<bb::GameControllerScript*>(c)) gc = x;
        if (bb::BaldiScript* x = dynamic_cast<bb::BaldiScript*>(c)) bs = x;
        if (bb::CameraScript* x = dynamic_cast<bb::CameraScript*>(c)) cs = x;
      }
    if (!ps || !gc || !bs || !cs) { fail("jumpscare-camera-sees-baldi", "missing scripts"); return; }
    gc->ActivateSpoopMode();
    for (int f = 0; f < 3; f++) eng::runFullFrame(1.0f / 50.0f);
    const char* others[] = { "Principal of the Thing", "Playtime", "Gotta Sweep", "Its a Bully", "Arts and Crafters", "1st Prize" };
    for (size_t i = 0; i < g.gos.size(); i++)
      for (size_t k = 0; k < sizeof(others) / sizeof(others[0]); k++)
        if (g.gos[i]->name == others[k]) g.gos[i]->SetActive(false);
    // Baldi 25 units away along a hall direction from the player (walkable: Warp snaps to the mesh)
    const float r = angles[a] * 0.0174533f;
    // the spawn is OUTSIDE the entrance (z -5, off the navmesh): stand in the x = 5 hall instead
    ps->transform->position = bb::Vec3(5.0f, ps->transform->position.y, 70.0f);
    for (int f = 0; f < 2; f++) eng::runFullFrame(1.0f / 50.0f);
    const bb::Vec3 p = ps->transform->position;
    bb::NavMeshAgent* ag = bs->gameObject->GetComponent<bb::NavMeshAgent>();
    ag->Warp(bb::Vec3(p.x + sinf(r) * 25.0f, 0.0f, p.z + cosf(r) * 25.0f));
    for (int f = 0; f < 50 * 40 && !ps->gameOver; f++) {
      if (f % 25 == 0) bs->Hear(ps->transform->position, 10.0f);   // keep him on the player
      eng::runFullFrame(1.0f / 50.0f);
      if (getenv("SIM_CHASE") && f % 100 == 0)
        printf("CHASE a=%d t=%.0f baldi=(%.1f,%.2f,%.1f) player=(%.1f,%.1f) d=%.2f active=%d speed=%.1f dest=(%.1f,%.1f) vel=%.1f\n", a, f / 50.0f,
               bs->transform->position.x, bs->transform->position.y, bs->transform->position.z, ps->transform->position.x,
               ps->transform->position.z, bb::length(bb::Vec3(bs->transform->position.x - ps->transform->position.x, 0.0f,
                                                              bs->transform->position.z - ps->transform->position.z)),
               (int)bs->gameObject->activeInHierarchy(), ag->speed, ag->destination().x, ag->destination().z, ag->velocity().magnitude());
    }
    if (!ps->gameOver) { detail += " [" + i2s((long long)angles[a]) + ": no catch]"; continue; }
    caught++;
    for (int f = 0; f < 3; f++) eng::runFullFrame(1.0f / 50.0f);   // LateUpdate places the camera
    const bb::Vec3 cam = cs->transform->position;
    const bb::Vec3 head(bs->transform->position.x, bs->transform->position.y + cs->BaldiOffset.y, bs->transform->position.z);
    bb::RaycastHit h;
    const bool hit = bb::Physics::Raycast(cam, head - cam, &h, bb::length(head - cam), ~0, bb::QueryTriggerInteraction::Ignore);
    const bb::Vec3 bf = bs->transform->forward();
    const bb::Vec3 toP = bb::normalized(bb::Vec3(ps->transform->position.x - bs->transform->position.x, 0.0f,
                                                 ps->transform->position.z - bs->transform->position.z));
    const bool clear = !hit;
    if (clear) seen++;
    char buf[200];
    snprintf(buf, sizeof buf, " [from %d deg: view %s, baldi.forward . toPlayer %.2f, cam-baldi %.1f u]",
             (int)angles[a], clear ? "clear" : (h.gameObject ? h.gameObject->name.c_str() : "blocked"),
             bb::dot(bf, toP), bb::length(head - cam));
    detail += buf;
  }
  if (getenv("SIM_CHASE")) printf("JUMPSCARE%s\n", detail.c_str());
  check("jumpscare-camera-sees-baldi", caught > 0 && seen == caught,
        i2s((long long)seen) + " of " + i2s((long long)caught) + " catches show Baldi:" + detail);
}


// The game-over jumpscare is BAL_Screech played ONCE, forward from the catch: the genuine game's
// sounds exactly like the impossible third ThinkPad question's single BAL_Screech (the user's
// listening comparison), though GameControllerScript restarts the clip every frame.
void testGameOverVoiceRule() {
  eng::unloadScene(true);
  eng::loadScene("School");
  for (int f = 0; f < 5; f++) eng::runFullFrame(1.0f / 50.0f);
  eng::State& g = eng::state();
  bb::PlayerScript* ps = 0;
  bb::GameControllerScript* gc = 0;
  for (size_t i = 0; i < g.gos.size(); i++) {
    if (!ps) ps = g.gos[i]->GetComponent<bb::PlayerScript>();
    if (!gc) gc = g.gos[i]->GetComponent<bb::GameControllerScript>();
  }
  if (!ps || !gc || !gc->aud_buzz) { fail("gameover-screech-single-copy", "no player / GC / aud_buzz"); return; }
  eng::sceneShown();   // release any scene-swap audio hold left by earlier swaps (no frames presented)
  gc->aud_buzz->loadPcm();
  const std::vector<int16_t>& clip = gc->aud_buzz->pcm;
  short buf[882 * 2];
  for (int f = 0; f < 5; f++) { eng::runFullFrame(1.0f / 50.0f); bb::audioPull(buf, 882); }
  std::vector<float> mono;
  ps->gameOver = true;
  for (int f = 0; f < 40; f++) {                     // 0.8 s at the shell's capture cadence
    eng::runFullFrame(1.0f / 50.0f);
    bb::audioPull(buf, 882);
    for (int i = 0; i < 882; i++) mono.push_back((float)buf[i * 2]);
  }
  const int seg = 2205, at = (int)(0.70f * 44100.0f);   // 50 ms, 0.7 s into the jumpscare
  int bestOff = -1;
  double best = -2.0;
  if ((int)mono.size() >= at + seg && clip.size() > (size_t)(0.45f * 44100.0f) + seg) {
    double ea = 0.0;
    for (int i = 0; i < seg; i++) ea += (double)mono[at + i] * mono[at + i];
    for (int off = 0; off < (int)(0.85f * 44100.0f); off += 1) {
      double dot = 0.0, eb = 0.0;
      for (int i = 0; i < seg; i++) { dot += (double)mono[at + i] * clip[off + i]; eb += (double)clip[off + i] * clip[off + i]; }
      const double cc = dot / (sqrt(ea * eb) + 1e-9);
      if (cc > best) { best = cc; bestOff = off; }
    }
  }
  check("gameover-screech-single-copy", bestOff >= (int)(0.62f * 44100.0f) && bestOff <= (int)(0.72f * 44100.0f) && best > 0.98,
        "0.7 s into the game over the mix best matches BAL_Screech at " + f2s(bestOff / 44100.0f) +
        " s (correlation " + f2s((float)best) + "); one copy playing forward: ~0.68-0.70 s, correlation ~1");
}


// Live LoadSceneAsync: the School is read + parsed on a worker while the MainMenu keeps running, so
// the LoadScreen's Baldi head (LAOD, an unscaled-time Animator over HeadSpin0000..) keeps spinning
// through the load instead of freezing on one frame (user report).
void testLoadScreenSpinsDuringLoad() {
  eng::State& g = eng::state();
  eng::unloadScene(true);
  eng::loadScene("MainMenu");
  for (int f = 0; f < 2; f++) eng::runFullFrame(1.0f / 60.0f);
  bb::GameObject* screen = 0, *laod = 0;
  for (size_t i = 0; i < g.gos.size(); i++) {
    if (g.gos[i]->name == "LoadScreen") screen = g.gos[i];
    if (g.gos[i]->name == "LAOD") laod = g.gos[i];
  }
  bb::Image* im = laod ? laod->GetComponent<bb::Image>() : 0;
  if (!screen || !im) { fail("load-screen-spins-during-load", "no LoadScreen / LAOD Image"); return; }
  screen->SetActive(true);
  g.asyncWaitAtSwap = false;                     // the live game's behaviour
  bb::LoadSceneAsync("School");
  std::set<const bb::Sprite*> frames;
  int menuFrames = 0;
  for (int f = 0; f < 600 && eng::currentSceneName() == "MainMenu"; f++) {
    frames.insert(im->sprite);
    menuFrames++;
    std::this_thread::sleep_for(std::chrono::milliseconds(16));   // a real 60 Hz frame's worth of time
    eng::runFullFrame(1.0f / 60.0f);
  }
  g.asyncWaitAtSwap = true;
  const bool swapped = eng::currentSceneName() == "School";
  check("load-screen-spins-during-load", swapped && menuFrames >= 2 && frames.size() >= 2,
        std::string(swapped ? "School loaded" : "School NOT loaded") + " after " + i2s(menuFrames) +
        " menu frames; the head showed " + i2s((long long)frames.size()) + " different HeadSpin frames meanwhile");
}


// Diagnostic (SIM_VOICES=1): how many voices a normal School keeps going (main + one-shots), the
// pool the 32-real-voice cap chooses from.
static void dumpVoiceCount() {
  eng::unloadScene(true);
  eng::loadScene("School");
  eng::sceneShown();
  eng::State& g = eng::state();
  short buf[1024 * 2];
  bb::GameControllerScript* gc = 0;
  for (size_t i = 0; i < g.gos.size() && !gc; i++) gc = g.gos[i]->GetComponent<bb::GameControllerScript>();
  for (int phase = 0; phase < 2; phase++) {
    if (phase == 1 && gc) gc->ActivateSpoopMode();
    for (int f = 0; f < 50 * 10; f++) {
      eng::runFullFrame(1.0f / 50.0f);
      bb::audioPull(buf, 882);
      if (f % 100 == 99) {
        int mains = 0, shots = 0, loops = 0;
        for (size_t i = 0; i < g.audioSources.size(); i++) {
          std::unordered_map<bb::AudioSource*, eng::AudioSim>::const_iterator it = g.audioSim.find(g.audioSources[i]);
          if (it == g.audioSim.end() || it->second.paused) continue;
          if (it->second.playing && g.audioSources[i]->clip && it->second.mainStart >= 0.0) {
            mains++;
            if (g.audioSources[i]->loop) loops++;
          }
          shots += (int)it->second.shots.size();
        }
        printf("VOICES %s t=%2.0fs: %d main (%d looping) + %d one-shots = %d (real cap 32)\n",
               phase ? "spoop " : "before", f / 50.0f + 0.02f, mains, loops, shots, mains + shots);
      }
    }
  }
}


// Crash in the user's play session (minidump: drawBillboards -> GameObject::activeInHierarchy on
// freed memory): destroyed objects' renderers stayed in the render lists. Throw a BSODA, let it
// destroy itself (BsodaSparyScript lifeSpan 30 s), then every listed renderer must belong to a
// live GameObject.
void testRenderListsAfterDestroy() {
  eng::unloadScene(true);
  eng::loadScene("School");
  for (int f = 0; f < 5; f++) eng::runFullFrame(1.0f / 50.0f);
  eng::State& g = eng::state();
  bb::GameControllerScript* gc = 0;
  for (size_t i = 0; i < g.gos.size() && !gc; i++) gc = g.gos[i]->GetComponent<bb::GameControllerScript>();
  if (!gc) { fail("render-lists-drop-destroyed", "no GameControllerScript"); return; }
  const size_t sr0 = g.spriteRenderers.size(), mr0 = g.meshRenderers.size();
  gc->item[0] = 4;
  gc->itemSelected = 0;
  gc->UseItem();                                  // the spray: a SpriteRenderer + a MeshRenderer
  eng::runFullFrame(1.0f / 50.0f);
  const size_t sr1 = g.spriteRenderers.size(), mr1 = g.meshRenderers.size();
  for (int f = 0; f < 50 * 32; f++) eng::runFullFrame(1.0f / 50.0f);   // past the 30 s lifespan
  std::set<const bb::GameObject*> live(g.gos.begin(), g.gos.end());
  int danglingS = 0, danglingM = 0;
  for (size_t i = 0; i < g.spriteRenderers.size(); i++)
    if (!live.count(g.spriteRenderers[i]->gameObject)) danglingS++;
  for (size_t i = 0; i < g.meshRenderers.size(); i++)
    if (!live.count(g.meshRenderers[i]->gameObject)) danglingM++;
  check("render-lists-drop-destroyed", sr1 > sr0 && mr1 > mr0 && danglingS == 0 && danglingM == 0,
        "spray added " + i2s((long long)(sr1 - sr0)) + " sprite / " + i2s((long long)(mr1 - mr0)) +
        " mesh renderers; after it destroyed itself: " + i2s(danglingS) + " sprite + " + i2s(danglingM) +
        " mesh renderers point at freed GameObjects (0)");
}


// The options turn-sensitivity slider must move under a press + drag (user: "i couldnt move the
// turn sensitivity slider at all"), its knob must follow, and OptionsManager.Update must store
// the value as MouseSensitivity.
void testTurnSliderDrag() {
  eng::State& g = eng::state();
  eng::unloadScene(true);
  eng::loadScene("MainMenu");
  for (int f = 0; f < 2; f++) eng::runFullFrame(1.0f / 60.0f);
  bb::Slider* sl = 0;
  for (size_t i = 0; i < g.gos.size() && !sl; i++)
    if (g.gos[i]->name == "TurnSlider") sl = g.gos[i]->GetComponent<bb::Slider>();
  if (!sl || !sl->handleRect) { fail("options-turn-slider-drags", "no TurnSlider / handle"); return; }
  // open the options screen: activate the slider's whole parent chain
  for (bb::Transform* t = sl->transform; t; t = t->parent) if (t->gameObject()) t->gameObject()->SetActive(true);
  eng::runFullFrame(1.0f / 60.0f);
  float x0, y0, x1, y1;
  bb::Transform* track = sl->handleRect->parent;
  if (!eng::uiScreenRect(track, x0, y0, x1, y1)) { fail("options-turn-slider-drags", "track not on screen"); return; }
  const float cy = (y0 + y1) * 0.5f;
  const bool grabbed = eng::uiSliderDown(5, x0 + (x1 - x0) * 0.25f, cy);
  const float at25 = sl->value;
  eng::uiSliderMove(5, x0 + (x1 - x0) * 0.90f, cy);
  const float at90 = sl->value;
  eng::uiSliderUp(5);
  eng::uiSliderMove(5, x0 + (x1 - x0) * 0.10f, cy);   // released: no longer follows
  const float after = sl->value;
  eng::clearDrawList();
  eng::buildDrawList();                                // Slider.UpdateVisuals moves the knob
  const float knob = sl->handleRect->anchorMin.x;
  eng::runFullFrame(1.0f / 60.0f);                     // OptionsManager.Update -> PlayerPrefs
  const float saved = bb::PlayerPrefs::GetFloat("MouseSensitivity");
  const float e25 = 0.1f + 0.25f * 9.9f, e90 = 0.1f + 0.90f * 9.9f;
  check("options-turn-slider-drags", grabbed && fabsf(at25 - e25) < 0.05f && fabsf(at90 - e90) < 0.05f &&
        fabsf(after - at90) < 1e-4f && fabsf(knob - 0.90f) < 0.01f && fabsf(saved - at90) < 1e-3f,
        std::string(grabbed ? "grabbed" : "NOT grabbed") + "; value at 25% " + f2s(at25) + " (" + f2s(e25) + "), at 90% " +
        f2s(at90) + " (" + f2s(e90) + "), after release " + f2s(after) + "; knob anchor " + f2s(knob) +
        " (0.90); MouseSensitivity saved " + f2s(saved));
  bb::PlayerPrefs::SetFloat("MouseSensitivity", 2.0f);
}


// The collider broadphase must change NOTHING but speed: random raycasts across the School give
// the same first hit (collider + distance) through the grid as through brute force, and no
// collider the grid treats as static moves during real play.
void testBroadphaseMatchesBrute() {
  eng::unloadScene(true);
  eng::loadScene("School");
  for (int f = 0; f < 5; f++) eng::runFullFrame(1.0f / 50.0f);
  unsigned seed = 12345u;
  struct R { static float next(unsigned& s) { s = s * 1664525u + 1013904223u; return (s >> 8) * (1.0f / 16777216.0f); } };
  const int N = 4000;
  std::vector<bb::Vec3> o(N), d(N);
  std::vector<int> masks(N);
  for (int i = 0; i < N; i++) {
    o[i] = bb::Vec3(-150.0f + 360.0f * R::next(seed), 0.5f + 9.0f * R::next(seed), -20.0f + 420.0f * R::next(seed));
    const float yaw = 6.2831853f * R::next(seed), pitch = (R::next(seed) - 0.5f) * 1.2f;
    d[i] = bb::Vec3(sinf(yaw) * cosf(pitch), sinf(pitch), cosf(yaw) * cosf(pitch));
    masks[i] = (i % 3 == 0) ? 769 : ~0;            // the NPCs' mask and the default
  }
  std::vector<const bb::Collider*> hitGrid(N), hitBrute(N);
  std::vector<float> distGrid(N), distBrute(N);
  for (int pass = 0; pass < 2; pass++) {
    eng::setBroadphaseBrute(pass == 1);
    for (int i = 0; i < N; i++) {
      bb::RaycastHit h;
      const bool any = bb::Physics::Raycast(o[i], d[i], &h, (i % 2) ? 60.0f : std::numeric_limits<float>::infinity(), masks[i],
                                            (i % 4 == 1) ? bb::QueryTriggerInteraction::Ignore : bb::QueryTriggerInteraction::UseGlobal);
      (pass ? hitBrute : hitGrid)[i] = any ? h.collider : 0;
      (pass ? distBrute : distGrid)[i] = any ? h.distance : -1.0f;
    }
  }
  eng::setBroadphaseBrute(false);
  int mismatch = 0, hits = 0, firstBad = -1;
  for (int i = 0; i < N; i++) {
    if (hitBrute[i]) hits++;
    if (hitGrid[i] != hitBrute[i] || distGrid[i] != distBrute[i]) { mismatch++; if (firstBad < 0) firstBad = i; }
  }
  check("broadphase-raycasts-match-brute", mismatch == 0 && hits > N / 2,
        i2s(N) + " random School raycasts (" + i2s(hits) + " hits): " + i2s(mismatch) + " differ from brute force" +
        (firstBad >= 0 ? " (first #" + i2s(firstBad) + ")" : std::string()));
}


// Diagnostic (SIM_AGENTS=1): which agents are active at the School start, and how far each moves.
static void dumpAgentMotion() {
  eng::unloadScene(true);
  eng::loadScene("School");
  eng::State& g = eng::state();
  std::vector<bb::Vec3> p0;
  for (int f = 0; f < 2; f++) eng::runFullFrame(1.0f / 50.0f);
  for (size_t i = 0; i < g.agents.size(); i++) p0.push_back(g.agents[i]->transform ? g.agents[i]->transform->position : bb::Vec3());
  for (int f = 0; f < 250; f++) eng::runFullFrame(1.0f / 50.0f);
  for (size_t i = 0; i < g.agents.size(); i++) {
    bb::NavMeshAgent* a = g.agents[i];
    const bool act = a->gameObject && a->gameObject->activeInHierarchy();
    printf("AGENT %-24s active=%d moved %.1f u in 5 s\n", a->gameObject ? a->gameObject->name.c_str() : "?", (int)act,
           a->transform ? bb::length(a->transform->position - p0[i]) : -1.0f);
  }
}


// Diagnostic (SIM_GAMEOVER_LIVE=<path>): the game-over mix with LIVE timing -- 60 Hz frames, the
// output device pulling independent 1024-sample blocks (the shell's waveOut feed), float path.
static void dumpGameOverLive(const char* path) {
  eng::unloadScene(true);
  eng::loadScene("School");
  eng::sceneShown();
  for (int f = 0; f < 5; f++) eng::runFullFrame(1.0f / 60.0f);
  eng::State& g = eng::state();
  bb::PlayerScript* ps = 0;
  for (size_t i = 0; i < g.gos.size() && !ps; i++) ps = g.gos[i]->GetComponent<bb::PlayerScript>();
  if (!ps) return;
  std::vector<short> cap;
  float blk[1024 * 2];
  double owed = 0.0;
  ps->gameOver = true;
  size_t shots = 0, distinct = 0;
  for (int f = 0; f < 60 * 2 && eng::currentSceneName() == "School"; f++) {
    eng::runFullFrame(1.0f / 60.0f);
    if (f == 50) {                                  // mid-jumpscare: what is stacked on the sources
      std::set<double> starts;
      for (size_t i = 0; i < g.audioSources.size(); i++) {
        std::unordered_map<bb::AudioSource*, eng::AudioSim>::const_iterator it = g.audioSim.find(g.audioSources[i]);
        if (it == g.audioSim.end()) continue;
        shots = (std::max)(shots, it->second.shots.size());
        for (size_t j = 0; j < it->second.shots.size(); j++) starts.insert(it->second.shots[j].startSamples);
      }
      distinct = starts.size();
    }
    owed += 44100.0 / 60.0;                       // the device consumes 735 samples per frame
    while (owed >= 1024.0) {
      owed -= 1024.0;
      bb::audioPullFloat(blk, 1024);
      for (int i = 0; i < 1024 * 2; i++) {
        const float v = blk[i] * 0.1f * 32767.0f;
        cap.push_back((short)(v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v));
      }
    }
  }
  FILE* wf = fopen(path, "wb");
  if (!wf) return;
  const unsigned int dataBytes = (unsigned int)(cap.size() * sizeof(short));
  unsigned char h[44];
  memcpy(h + 0, "RIFF", 4); *(unsigned int*)(h + 4) = 36 + dataBytes;
  memcpy(h + 8, "WAVEfmt ", 8); *(unsigned int*)(h + 16) = 16;
  *(unsigned short*)(h + 20) = 1; *(unsigned short*)(h + 22) = 2;
  *(unsigned int*)(h + 24) = 44100; *(unsigned int*)(h + 28) = 44100 * 4;
  *(unsigned short*)(h + 32) = 4; *(unsigned short*)(h + 34) = 16;
  memcpy(h + 36, "data", 4); *(unsigned int*)(h + 40) = dataBytes;
  fwrite(h, 1, 44, wf);
  if (!cap.empty()) fwrite(&cap[0], 1, dataBytes, wf);
  fclose(wf);
  printf("GAMEOVER LIVE %s: %u frames; one source holds %u one-shots with %u DISTINCT start samples\n", path,
         (unsigned)(cap.size() / 2), (unsigned)shots, (unsigned)distinct);
}

void testTriggerEvents() {
  // Trigger messages are dispatched from the FIXED step, and the fixed-step accumulator advances
  // by scaled dt -- so timeScale 0 means no steps and no triggers at all. The MathGame clone made
  // above called gc->ActivateLearningGame(), and GameControllerScript::Update pins
  // Time::timeScale to 0 while a learning game is active (that is how the game freezes the world
  // behind the math keypad). Restore it, or every check below silently reads zero.
  bb::Time::timeScale = 1.0f;
  check("trigger-fixed-steps-run", bb::Time::timeScale == 1.0f, "timeScale could not be restored");

  // A trigger volume carrying the probe, and a non-trigger "actor" capsule that moves through it.
  bb::GameObject* volume = makeProbeObject("ProbeTriggerVolume", bb::Vec3(500.0f, 500.0f, 500.0f));
  bb::CapsuleCollider* vol = volume->AddComponent<bb::CapsuleCollider>();
  vol->isTrigger = true;
  vol->radius = 1.0f;
  vol->height = 2.0f;
  TriggerProbe* probe = volume->AddComponent<TriggerProbe>();

  bb::GameObject* actor = makeProbeObject("ProbeActor", bb::Vec3(500.0f, 500.0f, 510.0f)); // far
  bb::CapsuleCollider* act = actor->AddComponent<bb::CapsuleCollider>();
  act->radius = 0.5f;
  act->height = 2.0f;
  // Unity sends trigger messages only when one side has a Rigidbody (or is a CharacterController):
  // first prove a STATIC actor gets none, then give it a kinematic body like the game's NPCs.
  actor->transform->position = bb::Vec3(500.0f, 500.0f, 500.0f);
  eng::runFullFrame(1.0f / 50.0f);
  eng::runFullFrame(1.0f / 50.0f);
  check("trigger-static-pair-silent", probe->enters == 0 && probe->stays == 0,
        "static trigger vs static capsule: enters=" + i2s(probe->enters) + " stays=" + i2s(probe->stays) + " (Unity: none)");
  actor->transform->position = bb::Vec3(500.0f, 500.0f, 510.0f);
  eng::runFullFrame(1.0f / 50.0f);
  actor->AddComponent<bb::Rigidbody>()->isKinematic = true;

  // Apart: one step, nothing fires.
  eng::runFullFrame(1.0f / 50.0f);
  check("trigger-no-event-when-apart", probe->enters == 0 && probe->exits == 0,
        "enters=" + i2s(probe->enters) + " exits=" + i2s(probe->exits) + " while 10u apart");

  // Move the actor onto the volume: exactly one Enter, and it must carry the other collider.
  actor->transform->position = bb::Vec3(500.0f, 500.0f, 500.0f);
  eng::runFullFrame(1.0f / 50.0f);
  check("trigger-enter-fires-once", probe->enters == 1,
        "enters=" + i2s(probe->enters) + " after moving into the volume (expected exactly 1)");
  check("trigger-enter-passes-other", probe->lastOther == (bb::Collider*)act,
        "OnTriggerEnter received the wrong collider");

  // Staying overlapped: Stay repeats, Enter does not.
  const int entersAfterEnter = probe->enters;
  eng::runFullFrame(1.0f / 50.0f);
  eng::runFullFrame(1.0f / 50.0f);
  check("trigger-stay-repeats", probe->stays >= 2,
        "stays=" + i2s(probe->stays) + " over two overlapping steps (expected >= 2)");
  check("trigger-enter-not-repeated", probe->enters == entersAfterEnter,
        "Enter fired again while still overlapping (enters=" + i2s(probe->enters) + ")");

  // Move away: exactly one Exit.
  actor->transform->position = bb::Vec3(500.0f, 500.0f, 510.0f);
  eng::runFullFrame(1.0f / 50.0f);
  check("trigger-exit-fires-once", probe->exits == 1,
        "exits=" + i2s(probe->exits) + " after leaving (expected exactly 1)");

  // Two non-triggers must NOT generate events (Unity needs a trigger on one side).
  act->isTrigger = false;
  vol->isTrigger = false;
  probe->enters = 0;
  actor->transform->position = bb::Vec3(500.0f, 500.0f, 500.0f);
  eng::runFullFrame(1.0f / 50.0f);
  check("trigger-needs-a-trigger-side", probe->enters == 0,
        "two non-trigger colliders produced " + i2s(probe->enters) + " Enter(s)");

  // Capsule-vs-box path (the game's trigger volumes are mostly boxes).
  vol->enabled = false;                      // retire the capsule volume
  bb::GameObject* boxVol = makeProbeObject("ProbeBoxVolume", bb::Vec3(600.0f, 600.0f, 600.0f));
  bb::BoxCollider* bx = boxVol->AddComponent<bb::BoxCollider>();
  bx->isTrigger = true;
  bx->size = bb::Vec3(4.0f, 4.0f, 4.0f);
  TriggerProbe* boxProbe = boxVol->AddComponent<TriggerProbe>();
  actor->transform->position = bb::Vec3(600.0f, 600.0f, 600.0f);
  eng::runFullFrame(1.0f / 50.0f);
  check("trigger-capsule-vs-box", boxProbe->enters == 1,
        "box trigger saw " + i2s(boxProbe->enters) + " Enter(s) for a capsule inside it");
  actor->transform->position = bb::Vec3(600.0f, 600.0f, 640.0f);
  eng::runFullFrame(1.0f / 50.0f);
  check("trigger-capsule-vs-box-exit", boxProbe->exits == 1,
        "box trigger saw " + i2s(boxProbe->exits) + " Exit(s) after the capsule left");

  // A destroyed collider must not be dispatched to (nor leave a dangling pair behind).
  actor->transform->position = bb::Vec3(600.0f, 600.0f, 600.0f);
  eng::runFullFrame(1.0f / 50.0f);
  eng::destroyGameObjectNow(actor);
  eng::runFullFrame(1.0f / 50.0f);
  eng::runFullFrame(1.0f / 50.0f);
  pass("trigger-survives-destroyed-collider");

  // ---- CharacterController collision (Move & depenetrate sweep) ----------------------------------
  // A thin wall box stops a straight Move at face-minus-radius, sliding preserves the full
  // tangential component, --debug's noclip walks straight through, and velocity() reports the
  // ACTUAL displacement (Unity parity: the stamina scripts read it as such).
  {
    bb::GameObject* wallGo = makeProbeObject("ProbeWall", bb::Vec3(800.0f, 1.0f, 2.0f));
    bb::BoxCollider* wall = wallGo->AddComponent<bb::BoxCollider>();
    wall->isTrigger = false;
    wall->size = bb::Vec3(8.0f, 4.0f, 0.2f);   // wall face plane at z = 2 - 0.1 = 1.9
    bb::GameObject* walkerGo = makeProbeObject("ProbeWalker", bb::Vec3(800.0f, 1.0f, 0.0f));
    bb::CharacterController* walker = walkerGo->AddComponent<bb::CharacterController>();
    walker->radius = 0.4f;
    walker->height = 2.0f;

    walkerGo->transform->position = bb::Vec3(800.0f, 1.0f, 0.0f);
    walker->Move(bb::Vec3(0.0f, 0.0f, 3.0f));
    const float zStop = walkerGo->transform->position.z;
    check("cc-move-blocks-wall", zStop > 1.30f && zStop < 1.60f,
          "pushing a 3.0-length Move into a face at 1.9 with radius 0.4 should stop near 1.5, "
          "got z=" + f2s(zStop));

    walkerGo->transform->position = bb::Vec3(800.0f, 1.0f, 0.0f);
    walker->Move(bb::Vec3(1.0f, 0.0f, 3.0f));
    const bb::Vec3 slide = walkerGo->transform->position;
    check("cc-move-slides-along-wall", slide.x > 0.95f && slide.z > 1.30f && slide.z < 1.60f,
          "diagonal Move into a wall keeps the tangential x and the same z stop "
          "(x=" + f2s(slide.x) + " z=" + f2s(slide.z) + ")");

    const float actualZ = slide.z;   // displacement of the blocked Move = its z stop
    const float vZ = walker->velocity().z * (1.0f / 50.0f);
    check("cc-velocity-is-actual", fabsf(vZ - actualZ) < 0.05f,
          "velocity()*dt should equal the actual (blocked) displacement: "
          "v=" + f2s(vZ) + " vs actual " + f2s(actualZ));

    eng::setDebugFlags(eng::kDebugNoClip);   // the --debug noclip bit
    walkerGo->transform->position = bb::Vec3(800.0f, 1.0f, 0.0f);
    walker->Move(bb::Vec3(0.0f, 0.0f, 4.0f));
    const float zFly = walkerGo->transform->position.z;
    eng::setDebugFlags(0);
    check("cc-noclip-passes-through", zFly > 3.9f,
          "with kDebugNoClip the same Move must land exactly on the request, got z=" + f2s(zFly));

    walkerGo->transform->position = bb::Vec3(800.0f, 1.0f, 0.0f);
    walker->Move(bb::Vec3(0.0f, 0.0f, 3.0f));
    const float zBack = walkerGo->transform->position.z;
    check("cc-blocks-again-after-flags-off", zBack > 1.30f && zBack < 1.60f,
          "after clearing the flags the blocker is live again, got z=" + f2s(zBack));

    eng::destroyGameObjectNow(walkerGo);
    eng::destroyGameObjectNow(wallGo);
    pass("cc-probes-cleaned");

    // ---- corner: collide-and-slide into a two-wall inside corner -----------------------------------
    // A user report: walking into a corner STICKS. Both walls are touched each frame; the slide
    // must clip wall A then wall B (expel-then-clip when the second contact is already inside
    // the band) and settle WITHOUT penetrating, then still let the player walk back out.
    {
      // inside corner whose open quadrant is -x/-z: wallA face z=902-0.1=901.9, wallB face x=901.9
      bb::GameObject* cGoA = makeProbeObject("ProbeCornerWallZ", bb::Vec3(901.0f, 1.0f, 902.0f));
      bb::BoxCollider* cA = cGoA->AddComponent<bb::BoxCollider>();
      cA->isTrigger = false;
      cA->size = bb::Vec3(8.0f, 4.0f, 0.2f);
      bb::GameObject* cGoB = makeProbeObject("ProbeCornerWallX", bb::Vec3(902.0f, 1.0f, 901.0f));
      bb::BoxCollider* cB = cGoB->AddComponent<bb::BoxCollider>();
      cB->isTrigger = false;
      cB->size = bb::Vec3(0.2f, 4.0f, 8.0f);
      bb::GameObject* cWalkerGo = makeProbeObject("ProbeCornerWalker", bb::Vec3(900.0f, 1.0f, 900.0f));
      bb::CharacterController* cWalker = cWalkerGo->AddComponent<bb::CharacterController>();
      cWalker->radius = 0.4f;
      cWalker->height = 2.0f;

      const bb::Vec3 step = bb::normalized(bb::Vec3(1.0f, 0.0f, 1.0f)) * 0.2f;
      cWalkerGo->transform->position = bb::Vec3(900.0f, 1.0f, 900.0f);
      for (int f = 0; f < 60; f++) cWalker->Move(step);
      const bb::Vec3 cEnd = cWalkerGo->transform->position;
      // radius 0.4 (rEff 0.32): stop planes are 901.9-0.32 = 1.58 -> (901.58, 901.58); deep enough
      // to have converged (`f=59` is far past the approach), shallow enough to prove no tunneling.
      check("cc-corner-walk-in-no-stick", cEnd.x > 901.30f && cEnd.x < 901.65f &&
            cEnd.z > 901.30f && cEnd.z < 901.65f,
            "60 diagonal steps into the corner left the capsule at (" + f2s(cEnd.x) + "," +
            f2s(cEnd.z) + "), expected converged near (901.58,901.58) -- tunneling or wedged");

      // walking BACK OUT of the corner must actually move (the stuck report)
      cWalker->Move(bb::normalized(bb::Vec3(-1.0f, 0.0f, -1.0f)) * 0.2f);
      const bb::Vec3 cOut = cWalkerGo->transform->position;
      // From the landing (901.58) one 0.2 diagonal step is 0.2/sqrt(2) = 0.1414 per axis, so a
      // FREE step ends at 901.4386. The old bound (< 901.40) needed 0.18 per axis -- more than a
      // whole step -- and could only ever pass from a WRONG landing point; it failed the moment
      // the capsule converged correctly. Require at least ~90% of the free step on each axis.
      check("cc-corner-walk-out", cOut.x < 901.453f && cOut.z < 901.453f,
            "one step back out of the corner only reached (" + f2s(cOut.x) + "," + f2s(cOut.z) +
            ") -- the capsule wedged instead of unwedging");

      eng::destroyGameObjectNow(cWalkerGo);
      eng::destroyGameObjectNow(cGoA);
      eng::destroyGameObjectNow(cGoB);
      pass("cc-corner-probes-cleaned");
    }
  }

  // ---- registry ordering ----------------------------------------------------------------------
  // g.gos IS the creation order that GameObject::Find and FindObjectOfType are specified to walk.
  // destroyObjectNow used a swap-remove, which moved the LAST object into the destroyed slot and
  // silently scrambled that order; three same-named objects make the regression observable.
  // These three are the newest entries in the registry, so they sit at its tail in creation order.
  bb::GameObject* ord1 = makeProbeObject("ProbeOrder", bb::Vec3(700.0f, 0.0f, 0.0f));
  bb::GameObject* ord2 = makeProbeObject("ProbeOrder", bb::Vec3(701.0f, 0.0f, 0.0f));
  bb::GameObject* ord3 = makeProbeObject("ProbeOrder", bb::Vec3(702.0f, 0.0f, 0.0f));
  check("find-returns-first-created", bb::GameObject::Find("ProbeOrder") == ord1,
        "Find did not return the first-created of three same-named objects");

  // Destroy the FIRST of the three -- the only case that distinguishes the two removal strategies.
  // Order-preserving erase leaves [ord2, ord3], so Find returns ord2. A swap-remove would move the
  // registry's LAST entry (ord3) into ord1's slot, leaving [ord3, ord2] and returning ord3.
  // (Destroying the middle one proves nothing: ord1 stays first either way.)
  eng::destroyGameObjectNow(ord1);
  check("find-order-survives-destroy", bb::GameObject::Find("ProbeOrder") == ord2,
        "after destroying the first of three, Find returned the wrong one -- creation order was "
        "scrambled by the removal (swap-remove instead of an order-preserving erase)");
  eng::destroyGameObjectNow(ord2);
  check("find-order-after-second-destroyed", bb::GameObject::Find("ProbeOrder") == ord3,
        "Find should now return the only remaining object");
  eng::destroyGameObjectNow(ord3);
}

// ---- mesh collider raycasts (built-in Quad / Plane surfaces) -----------------------------------
// Purpose-made panels far outside the level, mirroring the wall geometry School actually uses
// (scale-10 Quads). Pins: quad hit + collider identity, hit distance, off-panel and parallel
// misses, the documented DOUBLE-SIDED choice (a ray from the back side also counts as a hit --
// see the meshEntryT comment in eng-core), and a Plane under non-uniform scale (the scale fold's
// x/y/z path). Raycast is immediate, so no frames are needed here.
void testMeshRaycast() {
  const int kMask = ~0;
  const bb::QueryTriggerInteraction kQuery = bb::QueryTriggerInteraction::Collide;

  // Quad panel: local XY at z=0, scale (10,10,1) => a 10x10 world wall; pre-scale local
  // half-extents stay 0.5/0.5 exactly as bindBuiltin sets them.
  bb::GameObject* quadGo = makeProbeObject("ProbeQuadWall", bb::Vec3(800.0f, 100.0f, 0.0f));
  quadGo->transform->scale = bb::Vec3(10.0f, 10.0f, 1.0f);
  bb::MeshCollider* wall = quadGo->AddComponent<bb::MeshCollider>();
  wall->surface = bb::MeshCollider::SurfaceQuad;
  wall->halfU = 0.5f;
  wall->halfV = 0.5f;

  bb::RaycastHit qhit;
  bool qGot = bb::Physics::Raycast(bb::Vec3(800.0f, 100.0f, 10.0f), bb::Vec3(0.0f, 0.0f, -1.0f),
                                   &qhit, 100.0f, kMask, kQuery);
  check("mesh-quad-raycast-hits", qGot && qhit.collider == (bb::Collider*)wall,
        qGot ? "hit reported, but the collider is not the quad wall"
             : "front-face ray at the panel centre missed");

  // The panel sits at z=0 and the ray starts at z=10, so the hit must be 10u away.
  check("mesh-quad-raycast-distance", qGot && fabsf(qhit.distance - 10.0f) < 1e-3f,
        qGot ? "distance " + f2s(qhit.distance) + ", expected 10" : "no hit to measure");

  bb::RaycastHit trash;
  // Aim 5u past the panel edge (scale 10 puts the edge at x=805): a miss.
  bool qEdge = bb::Physics::Raycast(bb::Vec3(810.0f, 100.0f, 10.0f), bb::Vec3(0.0f, 0.0f, -1.0f),
                                    &trash, 100.0f, kMask, kQuery);
  check("mesh-quad-off-panel-misses", !qEdge, "a ray 5u beyond the panel edge hit the wall");

  // Parallel to the panel: never crosses its plane, so it must miss (dw guard in meshEntryT).
  bool qPar = bb::Physics::Raycast(bb::Vec3(800.0f, 100.0f, 10.0f), bb::Vec3(1.0f, 0.0f, 0.0f),
                                   &trash, 100.0f, kMask, kQuery);
  check("mesh-quad-parallel-misses", !qPar, "a ray parallel to the panel reported a hit");

  // From BEHIND: box/capsule tests are boundary crossings from any side, occlusion wants walls
  // to block both ways, so mesh surfaces are intentionally double-sided (not Unity's default
  // queriesHitBackfaces=false). This check pins that documented choice against silent drift.
  bool qBack = bb::Physics::Raycast(bb::Vec3(800.0f, 100.0f, -10.0f), bb::Vec3(0.0f, 0.0f, 1.0f),
                                    &trash, 100.0f, kMask, kQuery);
  check("mesh-quad-double-sided", qBack && trash.collider == (bb::Collider*)wall,
        "a back-side ray missed -- the double-sided choice regressed");

  // Plane floor under NON-UNIFORM scale: local XZ at y=0, half 5/5, scale (2,1,2) => a 10x10
  // world floor. Exercises compDiv's independent axis scaling inside meshEntryT.
  bb::GameObject* planeGo = makeProbeObject("ProbePlaneFloor", bb::Vec3(850.0f, 100.0f, 0.0f));
  planeGo->transform->scale = bb::Vec3(2.0f, 1.0f, 2.0f);
  bb::MeshCollider* floor = planeGo->AddComponent<bb::MeshCollider>();
  floor->surface = bb::MeshCollider::SurfacePlane;
  floor->halfU = 5.0f;
  floor->halfV = 5.0f;

  bb::RaycastHit phit;
  bool pGot = bb::Physics::Raycast(bb::Vec3(855.0f, 110.0f, 3.0f), bb::Vec3(0.0f, -1.0f, 0.0f),
                                   &phit, 200.0f, kMask, kQuery);
  check("mesh-plane-raycast-hits", pGot && phit.collider == (bb::Collider*)floor,
        pGot ? "hit reported, but the collider is not the plane floor"
             : "downward ray at the plane centre missed");
  check("mesh-plane-raycast-distance", pGot && fabsf(phit.distance - 10.0f) < 1e-3f,
        pGot ? "distance " + f2s(phit.distance) + ", expected 10 (plane at y=100)"
             : "no hit to measure");

  // World edge is x in [840, 860]: x=870 must miss.
  bool pEdge = bb::Physics::Raycast(bb::Vec3(870.0f, 110.0f, 3.0f), bb::Vec3(0.0f, -1.0f, 0.0f),
                                    &phit, 200.0f, kMask, kQuery);
  check("mesh-plane-off-panel-misses", !pEdge, "a ray 5u beyond the plane edge reported a hit");

  eng::destroyGameObjectNow(quadGo);
  eng::destroyGameObjectNow(planeGo);
}

// ---- mesh collider triggers (capsule-vs-quad / plane panels) -----------------------------------
// The 23 School wall triggers are zero-thickness quads; the overlap rule being pinned: a capsule
// overlaps the panel when its axis segment reaches within one radius of the panel plane WHILE
// in-bounds (the thin axis of the radius-expanded slab carries the whole crossing test). Built
// from probe objects, fixed-step driven. timeScale must be 1 (see testTriggerEvents' comment).
void testMeshTrigger() {
  bb::Time::timeScale = 1.0f;

  // A quad wall trigger at (900,100,0), scale (10,10,1) => world 10x10 wall; capsule actor.
  bb::GameObject* wallGo = makeProbeObject("ProbeQuadTrigger", bb::Vec3(900.0f, 100.0f, 0.0f));
  wallGo->transform->scale = bb::Vec3(10.0f, 10.0f, 1.0f);
  bb::MeshCollider* wall = wallGo->AddComponent<bb::MeshCollider>();
  wall->surface = bb::MeshCollider::SurfaceQuad;
  wall->halfU = 0.5f;
  wall->halfV = 0.5f;
  wall->isTrigger = true;
  TriggerProbe* wallProbe = wallGo->AddComponent<TriggerProbe>();

  bb::GameObject* actor = makeProbeObject("ProbeMeshActor", bb::Vec3(900.0f, 100.0f, 20.0f));
  bb::CapsuleCollider* act = actor->AddComponent<bb::CapsuleCollider>();
  act->radius = 0.5f;
  act->height = 2.0f;
  actor->AddComponent<bb::Rigidbody>()->isKinematic = true;   // trigger messages need a body

  // Apart (20u behind the panel): nothing fires.
  eng::runFullFrame(1.0f / 50.0f);
  check("mesh-trigger-apart-quiet", wallProbe->enters == 0 && wallProbe->exits == 0,
        "enters=" + i2s(wallProbe->enters) + " exits=" + i2s(wallProbe->exits) + " while 20u away");

  // Through the panel: the capsule's axis pierces the quad plane in-bounds => exactly one Enter,
  // carrying the wall's MeshCollider.
  actor->transform->position = bb::Vec3(900.0f, 100.0f, 0.0f);
  eng::runFullFrame(1.0f / 50.0f);
  check("mesh-trigger-quad-enters-once", wallProbe->enters == 1,
        "enters=" + i2s(wallProbe->enters) + " after piercing the panel (expected exactly 1)");
  check("mesh-trigger-passes-collider", wallProbe->lastOther == (bb::Collider*)act,
        "OnTriggerEnter on the panel must receive the ACTOR's capsule (both sides of the pair "
        "receive the other's collider)");

  // Grazing alongside one radius from the plane: still overlapping => Stay repeats, Enter not.
  actor->transform->position = bb::Vec3(900.0f, 100.0f, 0.4f);   // 0.4 < radius 0.5
  const int entersAfterEnter = wallProbe->enters;
  eng::runFullFrame(1.0f / 50.0f);
  eng::runFullFrame(1.0f / 50.0f);
  check("mesh-trigger-stay-repeats", wallProbe->stays >= 2,
        "stays=" + i2s(wallProbe->stays) + " while grazing within one radius (expected >= 2)");
  check("mesh-trigger-enter-not-repeated", wallProbe->enters == entersAfterEnter,
        "Enter fired again while still grazing (enters=" + i2s(wallProbe->enters) + ")");

  // Past the edge, same plane-depth: out-of-bounds u means NO overlap (thin axis alone is not it).
  actor->transform->position = bb::Vec3(910.0f, 100.0f, 0.0f);   // world edge |dx| from 900 is 10 > 5
  eng::runFullFrame(1.0f / 50.0f);
  check("mesh-trigger-beyond-edge-exits", wallProbe->exits == 1 && wallProbe->enters == entersAfterEnter,
        "stepping beyond the panel edge must produce exactly one Exit (exits=" +
        i2s(wallProbe->exits) + " enters=" + i2s(wallProbe->enters) + ")");

  // Far side of the wall after coming back in and leaving again: still exactly ordering-safe.
  actor->transform->position = bb::Vec3(900.0f, 100.0f, 0.0f);
  eng::runFullFrame(1.0f / 50.0f);
  const int entersSecond = wallProbe->enters;
  actor->transform->position = bb::Vec3(900.0f, 100.0f, -20.0f);
  eng::runFullFrame(1.0f / 50.0f);
  check("mesh-trigger-exit-on-separation", wallProbe->exits == 2 && wallProbe->enters == entersSecond,
        "re-enter then leave must add exactly one more Enter and one more Exit (exits=" +
        i2s(wallProbe->exits) + " enters=" + i2s(wallProbe->enters) + ")");

  eng::destroyGameObjectNow(wallGo);

  // A non-trigger quad (the other 2595 School walls) must generate NOTHING. The actor below is
  // still alive; it is finally retired after the plane check.
  bb::GameObject* inertGo = makeProbeObject("ProbeQuadWall", bb::Vec3(1000.0f, 100.0f, 0.0f));
  inertGo->transform->scale = bb::Vec3(10.0f, 10.0f, 1.0f);
  bb::MeshCollider* inert = inertGo->AddComponent<bb::MeshCollider>();
  inert->surface = bb::MeshCollider::SurfaceQuad;
  inert->halfU = 0.5f;
  inert->halfV = 0.5f;
  TriggerProbe* inertProbe = inertGo->AddComponent<TriggerProbe>();
  actor->transform->position = bb::Vec3(1000.0f, 100.0f, 0.0f);
  eng::runFullFrame(1.0f / 50.0f);
  check("mesh-trigger-wall-quad-inert", inertProbe->enters == 0 && inertProbe->exits == 0,
        "a non-trigger wall quad produced events (enters=" + i2s(inertProbe->enters) +
        " exits=" + i2s(inertProbe->exits) + ")");
  eng::destroyGameObjectNow(inertGo);

  // Plane trigger under its native scale: drop the actor onto the floor, then lift away.
  bb::GameObject* floorGo = makeProbeObject("ProbePlaneTrigger", bb::Vec3(950.0f, 100.0f, 0.0f));
  bb::MeshCollider* floor = floorGo->AddComponent<bb::MeshCollider>();
  floor->surface = bb::MeshCollider::SurfacePlane;
  floor->halfU = 5.0f;
  floor->halfV = 5.0f;
  floor->isTrigger = true;
  TriggerProbe* floorProbe = floorGo->AddComponent<TriggerProbe>();
  actor->transform->position = bb::Vec3(950.0f, 100.0f, 0.0f);   // axis on the plane, in-bounds
  eng::runFullFrame(1.0f / 50.0f);
  check("mesh-trigger-plane-enters", floorProbe->enters == 1 &&
        floorProbe->lastOther == (bb::Collider*)act,
        "plane floor didn't produce the expected single Enter with the actor capsule as other");
  actor->transform->position = bb::Vec3(950.0f, 120.0f, 0.0f);
  eng::runFullFrame(1.0f / 50.0f);
  check("mesh-trigger-plane-exits", floorProbe->exits == 1,
        "lifting away from the plane must produce exactly one Exit");
  eng::destroyGameObjectNow(floorGo);
  eng::destroyGameObjectNow(actor);
}

// ---- input: the real InputManager (InputManager.cs translation, engine key feed) --------------
// The DDoL input singleton now binds real actions: its Update() refreshes per-frame action
// states from bb::Input::GetKey (held) + simulated keys, and gameplay scripts (Player/Door/GC)
// route through Singleton<InputManager>::Instance(). These checks drive the ENGINE's injection
// surface exactly as the future SDL testbed layer will, and make the player actually WALK.
void testInputManager() {
  bb::Time::timeScale = 1.0f;

  bb::InputManager* im = bb::Singleton<bb::InputManager>::Instance();
  check("input-singleton-present", im != 0,
        "Singleton<InputManager>::Instance() null (auto-create via AddComponent->Awake broke)");
  if (!im) return;

  // Default bindings loaded on Awake: Run = LeftShift, Interact = secondary Mouse0.
  check("input-default-run-binding", im->ConvertInputActionToString(bb::InputAction::Run) == "LeftShift",
        "Run bound to '" + im->ConvertInputActionToString(bb::InputAction::Run) +
        "' (C# default GetDefaultBinding: LeftShift)");
  check("input-default-interact-binding",
        im->ConvertInputActionToString(bb::InputAction::Interact) == "Left Mouse Button",
        "Interact bound to '" + im->ConvertInputActionToString(bb::InputAction::Interact) +
        "' (C# default: KeyCode.None secondary Mouse0)");

  // Press/refresh/edge semantics: one inject produces exactly one GetActionKeyDown (the frame
  // after refresh), held for the press duration, one GetActionKeyUp after release.
  eng::injectKey((int)bb::KeyCode::LeftShift, true);
  eng::runFullFrame(1.0f / 50.0f);
  check("input-press-holds", im->GetActionKey(bb::InputAction::Run) &&
        im->GetActionKeyDown(bb::InputAction::Run),
        "LeftShift down did not register as a fresh Run press (key="
        + std::string(im->GetActionKey(bb::InputAction::Run) ? "held" : "up") + ")");
  eng::runFullFrame(1.0f / 50.0f);
  check("input-press-edges-once", im->GetActionKey(bb::InputAction::Run) &&
        !im->GetActionKeyDown(bb::InputAction::Run),
        "Run press repeated its Down edge on the second held frame");
  eng::injectKeyRelease((int)bb::KeyCode::LeftShift);
  eng::runFullFrame(1.0f / 50.0f);
  check("input-release-reports-up", im->GetActionKeyUp(bb::InputAction::Run) &&
        !im->GetActionKey(bb::InputAction::Run),
        "releasing LeftShift did not produce exactly one Up edge with the held state cleared");

  // Save/Load round trip through PlayerPrefs (Unity: "InputBinding_<n>" = "<primary>,<secondary>").
  im->Modify(bb::InputAction::Slot0, bb::KeyCode::Alpha5);
  check("input-display-string", im->ConvertInputActionToString(bb::InputAction::Slot0) == "5",
        "Slot0 display '" + im->ConvertInputActionToString(bb::InputAction::Slot0) +
        "' (expected \"5\" for Alpha5)");
  im->Save();
  im->Modify(bb::InputAction::Slot0, bb::KeyCode::Alpha3);          // tamper after saving
  im->Load();                                                       // must restore from the INI
  check("input-save-load-roundtrip",
        im->ConvertInputActionToString(bb::InputAction::Slot0) == "5",
        "Save->Load did not restore Slot0 = Alpha5 (got '" +
        im->ConvertInputActionToString(bb::InputAction::Slot0) + "')");
  im->SetDefaults();
  check("input-defaults-restored",
        im->ConvertInputActionToString(bb::InputAction::Slot0) == "1",
        "SetDefaults did not restore Slot0 = Alpha1 (got '" +
        im->ConvertInputActionToString(bb::InputAction::Slot0) + "')");

  // THE parity proof: holding MoveForward (W) makes the player WALK. Motion is measured along
  // the transform's forward at press time, so gravity/fell motion cannot mask it.
  bb::PlayerScript* player = bb::FindObjectOfType<bb::PlayerScript>();
  if (player && player->transform) {
    const bb::Vec3 pos0 = player->transform->position;
    const bb::Vec3 fwd = player->transform->forward();
    eng::injectKey((int)bb::KeyCode::W, true);
    for (int i = 0; i < 16; i++) eng::runFullFrame(1.0f / 50.0f);
    eng::injectKeyRelease((int)bb::KeyCode::W);
    eng::runFullFrame(1.0f / 50.0f);
    const bb::Vec3 moved = player->transform->position - pos0;
    const float alongForward = bb::dot(moved, fwd);
    check("input-walks-forward", alongForward > 0.2f,
          "holding W moved the player " + f2s(alongForward) + "u along its forward axis "
          "(movement input wiring or the Player move path is broken)");
    bool finite = moved.x == moved.x && moved.y == moved.y && moved.z == moved.z;
    check("input-motion-finite", finite, "player motion went NaN while walking");
  } else {
    fail("input-walks-forward", "no PlayerScript to drive");
    fail("input-motion-finite", "no PlayerScript to drive");
  }
}

// ---- scene-swap ref sweep ----------------------------------------------------------------------
// "Null the DDoL script pointers that point into the destroyed scene" only applies if something
// SURVIVES the swap with object pointers. An audit of the whole port shows the survivor set is
// exactly ONE runtime-created object by design: Core\UI\Settings\ControlMapper\InputManager.cs
// derives Singleton<InputManager>, and every gameplay script (Player/Door/GC/Notebook/Pickup)
// routes input through Singleton<InputManager>::Instance(), which self-creates a
// DontDestroyOnLoad GameObject on first use -- Unity parity, reproduced here. That stub carries
// NO pointer fields (and its eventual real translation will hold only PlayerPrefs-backed key
// mappings, also pointer-free), so there is no dangling sweep to perform today. These checks
// pin that invariant; if a future DDoL port ever gains pointer fields, a generated per-class
// field sweep (tools/bind-codegen.js) must land with it.
void testSceneSwapSweep() {
  eng::State& g = eng::state();
  // The exact path the game's own scene changes take (processPendingScenes: keepDontDestroy=true).
  eng::unloadScene(true);

  // Exactly the input singleton survives -- a second or stranger survivor means a script is
  // self-DDoLing without its translation expecting it (a pointer-sweep gap by definition).
  std::string survivorNames;
  for (size_t i = 0; i < g.gos.size(); i++) {
    survivorNames += std::string(i ? ", " : " ") + "'" + g.gos[i]->name + "'";
  }
  bb::InputManager* im = g.gos.size() >= 1
                             ? dynamic_cast<bb::InputManager*>(g.gos[0]->components.empty()
                                                                   ? (bb::Component*)0
                                                                   : g.gos[0]->components[0])
                             : (bb::InputManager*)0;
  bool transformWired = im != 0 && im->transform != 0 && im->transform->go == g.gos[0];
  bool transformRegistered = transformWired && g.transforms.size() == 1 &&
                             g.transforms[0] == im->transform;
  check("swap-gos-exactly-ddol-singleton", g.gos.size() == 1 && g.gos[0]->name == "InputManager" &&
        g.gos[0]->dontDestroy && im != 0 && transformWired && transformRegistered,
        "survivor set after unloadScene(true):" + survivorNames +
        " (transform wired: " + (transformWired ? "yes" : "NO") +
        ", transform registered: " + (transformRegistered ? "yes" : "NO") +
        ") -- expected exactly the runtime-created DontDestroyOnLoad InputManager with a live, "
        "registry-visible Transform (Unity: every GameObject carries one from construction)");

  check("swap-es-current-nulled", bb::EventSystem::current() == 0,
        "EventSystem::current() dangles after its scene unloaded");

  check("swap-coroutines-purged", g.coroutines.empty(),
        "coroutines outlived their own scene (they hold script back-pointers)");

  // Every per-component registry prunes through destroyGameObjectNow -> eraseOne; the only
  // legitimate leftovers are the survivor's own Transform. Anything else here is a registry
  // that missed an unload path, i.e. a stale pointer holder.
  std::string leftovers;
  if (!g.boxColliders.empty()) leftovers += " boxColliders=" + i2s(g.boxColliders.size());
  if (!g.capsuleColliders.empty()) leftovers += " capsules=" + i2s(g.capsuleColliders.size());
  if (!g.meshColliders.empty()) leftovers += " meshColliders=" + i2s(g.meshColliders.size());
  if (g.transforms.size() != g.gos.size()) leftovers += " transforms=" + i2s(g.transforms.size());
  if (!g.audioSources.empty()) leftovers += " audioSources=" + i2s(g.audioSources.size());
  if (!g.cameras.empty()) leftovers += " cameras=" + i2s(g.cameras.size());
  if (!g.agents.empty()) leftovers += " agents=" + i2s(g.agents.size());
  if (!g.agentSim.empty()) leftovers += " agentSim=" + i2s(g.agentSim.size());
  check("swap-scene-registries-purged", leftovers.empty(),
        "scene unload left behind:" + (leftovers.empty() ? std::string(" (none)") : leftovers));

  // The inverse guarantee: persistent OWNED nodes must NOT have died with the scene.
  check("swap-persistent-registries-kept", !g.prefabTemplates.empty() && !g.materials.empty(),
        "School's asset/template nodes vanished with the scene unload (ownedObjects fidelity)");

  // Captured for the cross-loop identity check in main().
  g_ddolCapture = im;
}

// After all six scene swaps the SAME singleton object must still be alive and findable --
// DontDestroyOnLoad kept it, and no second instance got minted by the next scenes' first
// Instance() calls (Singleton::Awake's dedupe branch destroys any impostor).
void testDdolStillAlive() {
  bb::InputManager* now = findAnyIncludingInactive<bb::InputManager>();
  check("swap-ddol-singleton-persists", now != 0 && now == g_ddolCapture,
        "the InputManager DDoL did not survive all scene swaps as the same object");
}

// ---- uGUI render pass: font bake -> anchor math -> headless draw list ---------------------------
// Exercises the full pipeline end-to-end: baked TMP fonts (pixel-proven glyph metrics from
// font-bake.js), the RectTransform-derives-From-Transform union shape, the binder's label
// metadata (fontSize/color32 unpacking/alignment), Unity's anchor algebra and TMP's baseline
// layout, and the active-in-hierarchy gate. After its probes the test leaves the fresh-swap
// state so the scene loop below keeps its assumptions.
static // Glyph quads only. Since the draw list also carries uGUI GRAPHICS quads (Image/RawImage solid
// fills, emitted before the labels), every check about TEXT geometry must filter to the textured
// ones -- otherwise a panel behind the label counts as an extra "glyph" or an extra baseline row.
std::vector<eng::DrawQuad> glyphQuads() {
  const std::vector<eng::DrawQuad>& all = eng::state().drawList;
  std::vector<eng::DrawQuad> out;
  // texId < 0 means the FONT ATLAS; a uGUI graphic that found baked pixels is textured too,
  // so textured alone no longer identifies a glyph.
  for (size_t i = 0; i < all.size(); i++) if (all[i].textured && all[i].texId < 0) out.push_back(all[i]);
  return out;
}

void testUiRenderPass() {
  eng::State& g = eng::state();
  const char* kComicProGuid = "21749360ea10093439a1f9266c91c718";

  eng::unloadScene(true);
  eng::loadScene("School");

  // 1) baked font registry: pixel-backed COMIC_Pro with the exact authored glyph metrics
  eng::TMPFontData* font = eng::fontForGuid(kComicProGuid);
  const eng::TMPFontGlyph* ga = font ? font->glyphFor('A') : 0;
  const eng::TMPFontGlyph* gsp = font ? font->glyphFor(' ') : 0;
  check("ui-fonts-bake-load",
        font != 0 && font->name == "COMIC_Pro" && font->atlasW == 128 && font->atlasH == 256 &&
        font->rgba.size() == (size_t)(font->atlasW * font->atlasH * 4) && ga != 0 && gsp != 0 &&
        ga->adv == 17.0f && ga->yo == 18.0f && ga->xo == 1.0f && ga->w == 14.0f && ga->h == 18.0f &&
        gsp->adv == 7.0f,
        "fontForGuid(COMIC_Pro): name '" + (font ? font->name : std::string()) + "', atlas " +
        i2s(font ? font->atlasW : 0) + "x" + i2s(font ? font->atlasH : 0) + ", bytes " +
        i2s(font ? (long long)font->rgba.size() : -1) +
        (ga ? ", A: xadv " : ", missing A; xadv ") +
        (ga ? f2s(ga->adv) : "-") + " yo " + (ga ? f2s(ga->yo) : "-") +
        " expected adv17 yo18, space adv7 expected");

  // point scale = fontSize / pointSize (TMP's mapping from metadata to pixels)
  const float ps24 = font ? font->pointScaleFor(24.0f) : 0.0f;
  const float ps18 = font ? font->pointScaleFor(18.0f) : 0.0f;
  check("ui-font-pointscale",
        font != 0 && ps24 > 0.9999f && ps24 < 1.0001f && ps18 > 0.7499f && ps18 < 0.7501f,
        "pointScaleFor(24)=" + f2s(ps24) + " pointScaleFor(18)=" + f2s(ps18));

  // 2) every TMP label rides the Unity shape: transform non-null AND that transform IS its rect
  std::vector<eng::EngineUiText*> labels;
  for (size_t i = 0; i < g.gos.size(); i++) {
    for (size_t j = 0; j < g.gos[i]->components.size(); j++) {
      eng::EngineUiText* ui = dynamic_cast<eng::EngineUiText*>(g.gos[i]->components[j]);
      if (ui != 0) labels.push_back(ui);
    }
  }
  check("ui-labels-count", (int)labels.size() == 12,
        i2s((long long)labels.size()) + " EngineUiText comps in School (bake: 12 TextMeshProUGUI)");
  int badShape = 0;
  for (size_t i = 0; i < labels.size(); i++) {
    if (labels[i]->transform == 0 ||
        dynamic_cast<bb::RectTransform*>(labels[i]->transform) == 0) badShape++;
  }
  check("ui-labels-rect-transformed", badShape == 0,
        i2s(badShape) + " labels whose transform is missing or not the baked RectTransform "
        "(Unity: a UI object's transform IS its rect)");

  // 3) the pause 'Nothing' label: binder metadata + baked anchors (ids pinned from the bake)
  eng::EngineUiText* nothing = 0;
  for (size_t i = 0; i < labels.size(); i++) {
    if (labels[i]->text == "Nothing") {
      nothing = labels[i];
      break;
    }
  }
  if (nothing == 0) {
    fail("ui-label-found", "no School label with text 'Nothing' (bake doc 3176/go 1777)");
    eng::loadScene("School"); // keep the state sane for the caller's next steps
    return;
  }
  pass("ui-label-found");

  bb::RectTransform* nr = dynamic_cast<bb::RectTransform*>(nothing->transform);
  check("ui-label-metadata",
        nothing->fontSize == 18.0f && nothing->alignment == 260 && nothing->wordWrap &&
        nothing->colorR == 0.0f && nothing->colorG == 0.0f && nothing->colorB == 0.0f &&
        nothing->colorA == 1.0f && nothing->font == font,
        "fontSize " + f2s(nothing->fontSize) + " align " + i2s(nothing->alignment) + " color (" +
        f2s(nothing->colorR) + "," + f2s(nothing->colorG) + "," + f2s(nothing->colorB) + "," +
        f2s(nothing->colorA) + ") wordWrap " + (nothing->wordWrap ? "yes" : "no") +
        " font " + (nothing->font ? nothing->font->name : std::string("NULL")) +
        " -- bake: 18/260, opaque BLACK (0xff000000), COMIC_Pro");
  check("ui-rect-bakes",
        nr != 0 && nr->anchorMin.x == 1.0f && nr->anchorMin.y == 1.0f && nr->anchorMax.x == 1.0f &&
        nr->anchorMax.y == 1.0f && nr->sizeDelta.x == 200.0f && nr->sizeDelta.y == 50.0f &&
        nr->anchoredPosition.x == -10.0f && nr->anchoredPosition.y == -65.0f && nr->pivot.x == 1.0f &&
        nr->pivot.y == 1.0f,
        "rect " + (nr ? "anchors(" + f2s(nr->anchorMin.x) + "," + f2s(nr->anchorMin.y) + ")-(" +
        f2s(nr->anchorMax.x) + "," + f2s(nr->anchorMax.y) + ") size(" + f2s(nr->sizeDelta.x) + "," +
        f2s(nr->sizeDelta.y) + ") pos(" + f2s(nr->anchoredPosition.x) + "," +
        f2s(nr->anchoredPosition.y) + ") pivot(" + f2s(nr->pivot.x) + "," + f2s(nr->pivot.y) + ")"
        : "NULL") + " -- bake: (1,1)-(1,1) size 200x50 pos -10,-65 pivot (1,1)");

  // 4) draw list: the pause chain is inactive at bake, so its glyphs stay out until the chain
  // activates (Unity activeInHierarchy gating). Then the isolated 'Nothing' label is exactly the
  // seven N-o-t-h-i-n-g glyph quads, each carrying its glyph's atlas u0/width at pointScale 0.75.
  eng::clearDrawList();
  eng::buildDrawList();
  const long long hiddenQuads = (long long)g.drawList.size();
  bool chainWasHidden = false;
  std::vector<bb::GameObject*> flippedGOs;
  for (bb::Transform* t = nothing->transform; t != 0; t = t->parent) {
    if (t->go && !t->go->activeSelf) {
      chainWasHidden = true;
      flippedGOs.push_back(t->go);
      t->go->activeSelf = true;
    }
  }
  check("ui-drawlist-hidden-until-active", !chainWasHidden || hiddenQuads == 0,
        chainWasHidden ? (i2s(hiddenQuads) + " quads on the draw list while the label's pause "
                                       "chain was inactive (Unity hides them)")
                       : ("the pause chain was authored active (no gating visible: " +
                          i2s(hiddenQuads) + " pre-activation quads)"));

  // blank every other label so the next build isolates 'Nothing'
  std::vector<std::string> saved;
  for (size_t i = 0; i < labels.size(); i++) {
    saved.push_back(labels[i]->text);
    if (labels[i] != nothing) labels[i]->text.clear();
  }
  eng::clearDrawList();
  eng::buildDrawList();
  const std::vector<eng::DrawQuad> only = glyphQuads(); // snapshot (build clears each call)
  check("ui-drawlist-nothing-only", only.size() == 7,
        i2s(only.size()) + " quads for 'Nothing' expected the 7 glyphs N o t h i n g");
  bool quadsFaithful = only.size() == 7;
  // 'Nothing' (ItemText) lives on the Hud canvas: ScaleWithScreenSize, reference 640x480,
  // m_ScreenMatchMode 2. Unity's enum is {MatchWidthOrHeight = 0, Expand = 1, Shrink = 2}, so 2 is
  // SHRINK: scaleFactor = MAX(w/640, h/480), which is 2.0 at 1280x720 (not the 1.5 that Expand
  // would give -- getting that enum backwards is an easy and silent mistake).
  // The canvas is laid out in DESIGN units and rasterised at scaleFactor px per unit, so a
  // glyph's PIXEL width is w * pointScale * scaleFactor. Before the CanvasScaler landed that
  // factor was silently 1 and the whole HUD drew half size at 1280x720. Derived here rather than
  // hardcoded so the check stays honest at any screen size, and FAILS if the scaler regresses.
  const float sxH = (float)g.screenW / 640.0f, syH = (float)g.screenH / 480.0f;
  const float hudCsf = (sxH > syH) ? sxH : syH;
  const float ps = (font ? font->pointScaleFor(nothing->fontSize) : 1.0f) * hudCsf;
  if (quadsFaithful) {
    const char* chars = "Nothing";
    float prevX0 = -1e30f;
    for (size_t i = 0; i < only.size(); i++) {
      const eng::TMPFontGlyph* gl = font ? font->glyphFor((int)(unsigned char)chars[i]) : 0;
      const bool rowFits = only[i].y0 >= 0.0f && only[i].y1 <= (float)g.screenH + 1.0f;
      if (gl == 0 || !rowFits || (only[i].x1 - only[i].x0) - gl->w * ps > 0.01f ||
          only[i].u0 - gl->u0 != 0.0f || only[i].x0 < prevX0 - 0.01f) {
        quadsFaithful = false;
      }
      prevX0 = only[i].x0;
    }
  }
  check("ui-drawlist-glyph-fidelity", quadsFaithful,
        "each quad must carry its glyph's atlas u0 + w*pointScale ink, in pen order, inside the "
        "canvas rows");
  if (!quadsFaithful && font != 0) { // failure diagnostics: quad vs glyph expectations
    printf("  (fidelity debug: ps=%g)\n", (double)ps);
    const char* charsDb = "Nothing";
    for (size_t i = 0; i < only.size() && i < 8; i++) {
      const eng::TMPFontGlyph* gl2 = font->glyphFor((int)(unsigned char)charsDb[i]);
      printf("   q%d '%c' x0=%g x1=%g y0=%g y1=%g u0=%g v0=%g | glyph w=%g xo=%g yo=%g u0=%g v0=%g\n",
             (int)i, charsDb[i], only[i].x0, only[i].x1, only[i].y0, only[i].y1, only[i].u0,
             only[i].v0, gl2 ? gl2->w : -1.0f, gl2 ? gl2->xo : -1.0f, gl2 ? gl2->yo : -1.0f,
             gl2 ? gl2->u0 : -1.0f, gl2 ? gl2->v0 : -1.0f);
    }
  }

  // 5) word wrap: same label, oversized text -> multiple baseline rows inside the 200px rect
  nothing->text = "Software Software Software Software patience patience patience";
  eng::clearDrawList();
  eng::buildDrawList();
  int rows = 0;
  std::vector<float> seen;
  {
    std::vector<float> ys;
    const std::vector<eng::DrawQuad> gq = glyphQuads();
    for (size_t i = 0; i < gq.size(); i++) ys.push_back(gq[i].y0);
    std::sort(ys.begin(), ys.end());
    for (size_t i = 0; i < ys.size(); i++) {
      if (i == 0 || ys[i] - seen.back() > font->lineHeight * ps * 0.5f) seen.push_back(ys[i]);
    }
    rows = (int)seen.size();
  }
  check("ui-drawlist-wrap", rows >= 2 && rows <= 5,
        i2s(rows) + " baseline rows for the oversized test string inside a 200px rect "
        "(wordWrap=1 -> 2..5 lines at 24.75 line height)");
  nothing->text = "Nothing";
  for (size_t i = 0; i < labels.size(); i++) labels[i]->text = saved[i];
  for (size_t i = 0; i < flippedGOs.size(); i++) flippedGOs[i]->activeSelf = false;

  // 6) MainMenu is the uGUI-heavy scene: 67 labels (66 COMIC_Pro with ink, one atlas-less COMIC
  // label that renders no quads -- advance/metrics only, documented limitation). The bake stores
  // every menu GO disabled until the game's own scripts enable it at runtime (Unity UIManager
  // flow), so the as-baked scene legitimately renders nothing (exact Unity parity: trace shows
  // PlayMenu/LoadScreen/MobileControlsMenu all [off]). Simulate the opened menu by flipping the
  // hierarchy active (activeInHierarchy), build, assert the full flood, then restore every flag.
  eng::unloadScene(true);
  eng::loadScene("MainMenu");
  // First run (no OptionsSet key): DefaultSettingsScript.Start disables the menu Canvas for ONE
  // frame (CloseOptions re-enables it after WaitForEndOfFrame) -- a disabled Canvas draws nothing,
  // so count the menu from the second frame, as it first appears in the genuine game.
  eng::runFullFrame(1.0f / 50.0f);
  std::vector<bb::GameObject*> menuSet;
  std::vector<bool> menuPrev;
  for (size_t i = 0; i < g.gos.size(); i++) {
    menuSet.push_back(g.gos[i]);
    menuPrev.push_back(g.gos[i]->activeSelf);
    g.gos[i]->activeSelf = true;
  }
  eng::clearDrawList();
  eng::buildDrawList();
  const long long mmQuads = (long long)g.drawList.size();
  long long mmGlyphs = 0;
  for (size_t i = 0; i < g.drawList.size(); i++)
    if (g.drawList[i].textured && g.drawList[i].texId < 0) mmGlyphs++;
  const long long mmGraphics = mmQuads - mmGlyphs;
  bool uvSane = true;
  for (size_t i = 0; i < g.drawList.size(); i++) {
    const eng::DrawQuad& q = g.drawList[i];
    if (q.u0 < 0.0f || q.u0 > 1.0f || q.v0 < 0.0f || q.v0 > 1.0f || q.u1 > 1.0001f ||
        q.v1 > 1.0001f) uvSane = false;
  }
  for (size_t i = 0; i < menuSet.size(); i++) menuSet[i]->activeSelf = menuPrev[i];

  // HIERARCHY DRAW ORDER. Unity paints a canvas depth-first by sibling index. Under the MainMenu
  // canvas, Background is rootOrder 0 and must paint FIRST; it is a sprite-less Image, i.e. an
  // opaque full-screen fill, so when the list was in creation order it emitted LAST and buried the
  // title and every button -- the menu rendered as a blank white screen. The first graphic quad
  // must therefore be the untextured full-screen one.
  eng::clearDrawList();
  eng::buildDrawList();
  bool bgFirst = false;
  for (size_t i = 0; i < g.drawList.size(); i++) {
    const eng::DrawQuad& q = g.drawList[i];
    if (q.texId < 0 && q.textured) continue;        // glyph quad: the label pass runs later anyway
    bgFirst = !q.textured && q.x0 <= 0.5f && q.y0 <= 0.5f &&
              q.x1 >= (float)g.screenW - 0.5f && q.y1 >= (float)g.screenH - 0.5f;
    break;                                          // only the FIRST graphic quad is the test
  }
  check("ui-drawlist-hierarchy-order", bgFirst,
        "the first MainMenu graphic must be the full-screen Background (rootOrder 0); if it is "
        "not, the draw list is in creation order and the opaque background buries the menu");

  // Split: graphics (images, fills) keep the old bound; GLYPHS are counted apart. Before every font
  // had atlas pixels (LiberationSans SDF / COMIC were metrics-only and drew nothing), the total sat
  // near 205; with them the glyph count alone is ~1300, and it moves whenever text does.
  printf("ui-drawlist-mainmenu: %lld quads = %lld graphics + %lld glyphs\n", mmQuads, mmGraphics, mmGlyphs);
  check("ui-drawlist-mainmenu", mmGraphics >= 100 && mmGraphics <= 400 && mmGlyphs > 200 && uvSane,
        i2s(mmGraphics) + " graphic quads (want 100..400) + " + i2s(mmGlyphs) +
        " glyph quads (want > 200) with the full menu hierarchy active, UVs " + (uvSane ? "sane" : "OUT OF RANGE"));

  eng::unloadScene(true); // hand the fresh-swap state back to the scene loop below
}

// ---- baked .asset mesh blobs -----------------------------------------------------------------
// These assert the REPRESENTATION, not just that something loaded. The euler bug (session 7s/7t)
// was invisible for three sessions because the only check compared a rotation to itself through
// two conversions of the same family; the lesson recorded there is that a decoded value must be
// pinned against an INDEPENDENT fact. The independent fact here is geometry: Unity's uv v=1 is the
// TOP of an image, so on an upright wall-like mesh v=1 must land on the vertices with the greatest
// local Y. That is what licenses the backends' t = 1-v flip (the baked texture blobs store row 0 =
// image top), and nothing in a tiling wood/metal texture would ever show it being wrong by eye.
void testMeshBlobs() {
  // CombinedMeshes_Locker: a single upright 2x8 quad, the simplest mesh in the game.
  const eng::MeshAsset* locker = eng::meshForGuid("403379e2f4627a74ea7c902e393d3b4a");
  check("mesh-blob-loads", locker != 0,
        "data/mesh/403379e2f4627a74ea7c902e393d3b4a.mesh (CombinedMeshes_Locker) did not load "
        "-- run: node tools\\bake-meshes.js");
  if (!locker) return;
  check("mesh-blob-counts", locker->vertCount() == 4 && locker->triCount() == 2,
        i2s((long long)locker->vertCount()) + " verts / " + i2s((long long)locker->triCount()) +
        " tris (CombinedMeshes_Locker is 4 verts / 2 tris in the asset)");

  float yAtV0 = 1e9f, yAtV1 = -1e9f;
  int badUv = 0, badIdx = 0;
  for (size_t i = 0; i < locker->vertCount(); i++) {
    const float* vp = &locker->verts[i * 5];
    const float y = vp[1], v = vp[4];
    if (vp[3] < -0.001f || vp[3] > 1.001f || v < -0.001f || v > 1.001f) badUv++;
    if (v > 0.5f) { if (y < yAtV1 || yAtV1 < -1e8f) yAtV1 = y; }
    else          { if (y > yAtV0 || yAtV0 > 1e8f) yAtV0 = y; }
  }
  for (size_t i = 0; i < locker->indices.size(); i++) {
    if ((size_t)locker->indices[i] >= locker->vertCount()) badIdx++;
  }
  check("mesh-blob-uv-range", badUv == 0, i2s(badUv) + " vertices with a uv outside [0,1]");
  check("mesh-blob-index-range", badIdx == 0,
        i2s(badIdx) + " indices past the end of the vertex array (a GPU fault in a backend)");
  // THE representation check: v=1 is geometrically ABOVE v=0.
  check("mesh-blob-uv-v-is-up", yAtV1 > yAtV0,
        "uv v=1 sits at y=" + f2s(yAtV1) + " and v=0 at y=" + f2s(yAtV0) +
        " -- Unity's v=1 is the image TOP, so it must be the geometric top; if this inverts, the "
        "backends' t = 1-v flip is upside down");

  // The 8 baked meshes must all load and be internally consistent, not just the one probed above.
  const char* kGuids[8] = {
    "403379e2f4627a74ea7c902e393d3b4a", "8a53dfed02cb1854683f7a67b28c3007",
    "8d7804b62ce15db41b7e95f79902dd7f", "a257ef97bb7fabf4fb03653406d0c797",
    "059e8dcd3ff2e8b4dad7b1d47a06bdc8", "57b0b49a70ddf904d9e63ea23fde74bb",
    "34c09060517cf16429480c15526de6c7", "a16d8074f7cabee40a6217f16fea9dec"
  };
  int loaded = 0, consistent = 0;
  for (int i = 0; i < 8; i++) {
    const eng::MeshAsset* m = eng::meshForGuid(kGuids[i]);
    if (!m) continue;
    loaded++;
    if (m->indices.size() % 3 == 0 && m->verts.size() % 5 == 0 && m->extent[1] > 0.0f) consistent++;
  }
  check("mesh-blobs-all-load", loaded == 8,
        i2s(loaded) + "/8 baked meshes loaded from data/mesh/");
  check("mesh-blobs-consistent", consistent == 8,
        i2s(consistent) + "/8 baked meshes are triangle lists with a non-degenerate AABB");
  // A guid with no blob must return 0, NOT a fallback: School has one such reference and a
  // substituted primitive there drew a giant grey plane through the room.
  check("mesh-blob-missing-is-null", eng::meshForGuid("00000000000000000000000000000000") == 0,
        "an unknown mesh guid must resolve to null so the renderer skips it");
}

// ---- world-render data (MeshRenderer owners, mesh primitives, material textures) --------------
// Groundwork for the 3D pass: a MeshRenderer is Object-derived and therefore NOT reachable via
// go->components, so without the registry and the owner pointers there is literally nothing for a
// renderer to iterate. These pin that wiring before any drawing code depends on it.
void testWorldRenderData() {
  eng::State& g = eng::state();
  const size_t n = g.meshRenderers.size();
  // School is built from scaled quads: 2721 MeshFilters in the bake, so the renderer count is
  // the same order. A floor here means the registry never filled.
  check("world-meshrenderers-registered", n > 2000,
        i2s((long long)n) + " MeshRenderers registered (expected >2000 for School)");

  int noOwner = 0, noTransform = 0, noMesh = 0, withTex = 0, quads = 0, planes = 0;
  int assets = 0, assetsNoGuid = 0, assetsResolved = 0, assetTris = 0;
  for (size_t i = 0; i < n; i++) {
    bb::MeshRenderer* mr = g.meshRenderers[i];
    if (!mr->gameObject) noOwner++;
    if (!mr->transform) noTransform++;
    if (mr->meshPrimitive == 0) noMesh++;
    else if (mr->meshPrimitive == 10210) quads++;
    else if (mr->meshPrimitive == 10209) planes++;
    else if (mr->meshPrimitive == 4300000) {
      assets++;
      if (mr->meshGuid.empty()) assetsNoGuid++;
      else {
        const eng::MeshAsset* m = eng::meshForGuid(mr->meshGuid);
        if (m) { assetsResolved++; assetTris += (int)m->triCount(); }
      }
    }
    if (mr->material && !mr->material->mainTexGuid.empty()) withTex++;
  }
  check("world-meshrenderer-owners", noOwner == 0,
        i2s(noOwner) + " MeshRenderers with no owning GameObject");
  // UI objects legitimately have no bb::Transform (RectTransform only), but a MeshRenderer never
  // sits on one, so any missing transform here is a wiring fault.
  check("world-meshrenderer-transforms", noTransform == 0,
        i2s(noTransform) + " MeshRenderers with no transform (nothing to place the quad with)");
  check("world-mesh-primitives", quads > 2000 && planes >= 0,
        i2s(quads) + " Quad / " + i2s(planes) + " Plane / " + i2s(noMesh) +
        " unrecognised -- School is built from scaled built-in Quads (10210)");
  // ---- real .asset meshes (desks / chairs / lockers / BSODA sides) ---------------------------
  // fileID 4300000 is shared by EVERY .asset mesh, so the guid is the only thing that says which
  // one. A renderer that lost its guid would silently fall back to "skip" and the classrooms would
  // come back empty -- that is the failure this pins.
  // Unity activeInHierarchy, NOT activeSelf. School ships disabled prototype content: an
  // `Old/PrototypeStuff` subtree with a `Locker` under it, both inactive, whose LEAF renderers are
  // all activeSelf == true. Testing the leaf flag alone drew a flat-red prototype locker 15 units
  // in front of the player spawn -- visible in every screenshot until session 7y. The count must
  // stay non-zero: if it hits 0 the case has vanished from the scene and this check is dead wood.
  int selfActiveButHidden = 0;
  for (size_t i = 0; i < n; i++) {
    bb::MeshRenderer* mr = g.meshRenderers[i];
    if (!mr->gameObject) continue;
    if (mr->gameObject->activeSelf && !mr->gameObject->activeInHierarchy()) selfActiveButHidden++;
  }
  check("world-active-in-hierarchy", selfActiveButHidden > 0,
        i2s(selfActiveButHidden) + " renderers are activeSelf but have a disabled ANCESTOR "
        "(School's Old/PrototypeStuff). A render pass must skip these; if this is 0 the scene no "
        "longer exercises the rule and the check needs a new case");

  check("world-asset-mesh-guids", assets > 700 && assetsNoGuid == 0,
        i2s(assets) + " renderers use a .asset mesh (4300000), " + i2s(assetsNoGuid) +
        " of them with no guid -- the guid is what identifies WHICH mesh");
  // School references 9 distinct mesh guids and the decompile ships 8; the ninth
  // (4d7c1677b1627914cbcfa55acd72889a, 1 renderer) is absent from the project and is SKIPPED by
  // design, so all but that one must resolve.
  check("world-asset-meshes-resolve", assetsResolved == assets - 1,
        i2s(assetsResolved) + "/" + i2s(assets) + " .asset mesh renderers resolved to a baked blob "
        "(exactly 1 unresolvable guid is expected: it is not in the decompile)");
  // 18200 triangles in School today. The floor is the MEASURED total minus a small margin,
  // not a guessed round number: it is here to catch a mesh silently dropping out of the
  // resolve path, and a guessed threshold would either fail on sight or never fire.
  check("world-asset-mesh-triangles", assetTris > 18000,
        i2s(assetTris) + " triangles across the resolved .asset meshes (18200 expected; "
        "the 252 lockers are 2-4 tris each, the 13 BSODA sides 320 each)");

  // ---- billboards (SpriteRenderer) -----------------------------------------------------------
  // Baldi, the NPCs, the pickups and the wall clutter are sprites, not geometry. The registry is
  // what a backend iterates; a SpriteRenderer with no sprite would silently draw nothing.
  const size_t sprN = g.spriteRenderers.size();
  int noSprite = 0, noGuid = 0, noXform = 0;
  for (size_t i = 0; i < sprN; i++) {
    bb::SpriteRenderer* sr = g.spriteRenderers[i];
    if (!sr->sprite) { noSprite++; continue; }
    if (sr->sprite->name.empty()) noGuid++;     // Sprite::name IS the texture guid
    if (!sr->transform) noXform++;
  }
  check("world-spriterenderers-registered", sprN > 100,
        i2s((long long)sprN) + " SpriteRenderers registered (School authors 131 billboards plus "
        "pickups; a 0 here means the registry never filled)");
  check("world-spriterenderer-sprites", noSprite == 0 && noGuid == 0,
        i2s(noSprite) + " with no sprite, " + i2s(noGuid) +
        " whose sprite carries no guid -- either draws nothing at all");
  check("world-spriterenderer-transforms", noXform == 0,
        i2s(noXform) + " SpriteRenderers with no transform (nothing to place the quad with)");

  // ---- animation clips (sprite keyframe tracks) -----------------------------------------------
  // The NPCs are animated by an ANIMATOR playing PPtr clips, not by script sprite arrays, which is
  // why the script-field sprite sweep never reached their frames. tools\bake-anims.js flattens the
  // clips to data/anims-index.json; this pins that the bake is present and self-consistent so the
  // runtime side can be written against it. It does NOT pin playback -- nothing plays these yet.
  {
    bool ok = false;
    const std::string text = eng::jsonReadFile("data/anims-index.json", ok);
    eng::JsonValue root;
    std::string err;
    const bool parsed = ok && eng::jsonParse(text, root, err);
    int clips = 0, loops = 0, emptyTrack = 0, keys = 0, outOfOrder = 0;
    if (parsed) {
      for (size_t i = 0; i < root.obj.size(); i++) {
        const eng::JsonValue& c = root.obj[i].second;
        clips++;
        const eng::JsonValue* lp = c.find("loop");
        if (lp && lp->asInt() != 0) loops++;
        const eng::JsonValue* tracks = c.find("tracks");
        if (!tracks || tracks->size() == 0) { emptyTrack++; continue; }
        for (size_t t = 0; t < tracks->size(); t++) {
          const eng::JsonValue* tr = tracks->at(t);
          const eng::JsonValue* ks = tr ? tr->find("keys") : 0;
          if (!ks || ks->size() == 0) { emptyTrack++; continue; }
          float prev = -1.0f;
          for (size_t k = 0; k < ks->size(); k++) {
            const eng::JsonValue* key = ks->at(k);
            const eng::JsonValue* tv = key ? key->find("t") : 0;
            const float tt = tv ? tv->asFloat(-1.0f) : -1.0f;
            // Keyframes must be non-negative and ascending: a player walks them in order.
            if (tt < prev) outOfOrder++;
            prev = tt;
            keys++;
          }
        }
      }
    }
    check("anim-clips-baked", parsed && clips >= 10 && emptyTrack == 0 && outOfOrder == 0,
          parsed ? (i2s(clips) + " clips / " + i2s(keys) + " sprite keys (" + i2s(loops) +
                    " looping), " + i2s(emptyTrack) + " empty tracks, " + i2s(outOfOrder) +
                    " keys out of time order -- run: node tools\\bake-anims.js")
                 : std::string("data/anims-index.json missing or unparsable -- run: node "
                               "tools\\bake-anims.js"));
  }

  // ---- uGUI graphics are in ROW space ---------------------------------------------------------
  // layRect is Unity's algebra and yields a DESIGN rect (y UP from the canvas bottom); DrawQuad is
  // row space (y DOWN from the top-left). The graphics pass used to emit the design y directly,
  // mirroring every Image about the screen's horizontal centre line, while the LABEL pass flipped
  // correctly -- so a label and its own background landed in opposite corners.
  // ItemSlots is the anchor case: anchorMin = anchorMax = pivot = (1,1), which is Unity's TOP
  // right, and in the shipped game the item slots are in the top right. Pinning a known corner
  // catches the mirror; a count or a uv range never could.
  eng::clearDrawList();
  eng::buildDrawList();
  bool slotsFound = false, slotsTopRight = false, slotsScaled = false;
  float slotsY0 = -1.0f, slotsW = 0.0f, slotsH = 0.0f;
  for (size_t i = 0; i < g.drawList.size(); i++) {
    const eng::DrawQuad& q = g.drawList[i];
    if (q.texId < 0 || (size_t)q.texId >= g.drawTextures.size()) continue;
    if (g.drawTextures[q.texId] != "fbbd39ec1ea56314583af635455848c8") continue;  // ItemSlots.png
    slotsFound = true;
    slotsY0 = q.y0;
    slotsW = q.x1 - q.x0;
    slotsH = q.y1 - q.y0;
    slotsTopRight = (q.y0 < (float)g.screenH * 0.25f) && (q.x1 > (float)g.screenW * 0.75f);
    // ItemSlots is 128x67 DESIGN units on the Hud canvas (ScaleWithScreenSize, reference
    // 640x480, m_ScreenMatchMode 2 = Shrink = MAX of the ratios), so its pixel size is
    // 128*csf x 67*csf = 256x134 at 1280x720. This is the CanvasScaler gate: the value is wrong
    // under every other scale mode, and it was 128x67 -- csf silently 1 -- before the scaler
    // was implemented, which drew the entire HUD at half size.
    const float csfX = (float)g.screenW / 640.0f, csfY = (float)g.screenH / 480.0f;
    const float csf = (csfX > csfY) ? csfX : csfY;
    slotsScaled = (fabsf(slotsW - 128.0f * csf) < 0.5f) && (fabsf(slotsH - 67.0f * csf) < 0.5f);
  }
  check("ui-canvas-scaler-applied", slotsFound && slotsScaled,
        slotsFound ? ("ItemSlots rasterised at " + f2s(slotsW) + "x" + f2s(slotsH) +
                      " px; it is 128x67 design units on a ScaleWithScreenSize 640x480 Shrink "
                      "canvas, so it must be 256x134 at 1280x720 (128x67 would mean scaleFactor "
                      "1, i.e. the CanvasScaler is being ignored)")
                   : std::string("no ItemSlots quad to measure"));
  // ONE ordering over both passes. buildDrawList used to emit every graphic and then every label,
  // which is only correct when every label sits above every panel -- it is wrong for a panel that
  // should cover text behind it. Both passes now share a single hierarchy sort, and in School that
  // genuinely interleaves: a glyph quad sorts BEFORE some graphics. If the passes are ever split
  // again, every glyph lands after every graphic and this goes red.
  // In School the hierarchy order puts the NotebookCount label (sibling 0 of the Hud) first and
  // the Boots graphic (sibling 8) last, so the list STARTS with a glyph and ENDS with a graphic.
  // Under the old split ordering -- every graphic, then every label -- both invert exactly. That
  // is what makes this a real gate rather than a restatement of "the list is non-empty".
  bool firstIsGlyph = false, lastIsGraphic = false;
  if (!g.drawList.empty()) {
    const eng::DrawQuad& f = g.drawList.front();
    const eng::DrawQuad& b = g.drawList.back();
    firstIsGlyph = f.textured && f.texId < 0;          // texId -1 + textured == the font atlas
    lastIsGraphic = !(b.textured && b.texId < 0);
  }
  // ---- Slider drives its fill rect (the stamina bar) -----------------------------------------
  // Unity's Slider recomputes fillRect's ANCHORS from the normalised value every frame and leaves
  // anchoredPosition/sizeDelta/pivot alone. Both halves matter and are pinned here by driving the
  // value to a known fraction and measuring the quads:
  //   - the green fill must start exactly where the red track starts (the Fill Area's +5 and the
  //     Fill's -5 anchoredPosition cancel; zeroing either slides the fill off the track), and
  //   - its width must be that fraction of the track (the anchors must actually be driven).
  {
    bb::Slider* stamina = 0;
    for (size_t i = 0; i < g.gos.size() && !stamina; i++) {
      if (g.gos[i]->name != "StaminaSlider") continue;
      for (size_t c = 0; c < g.gos[i]->components.size(); c++) {
        bb::Slider* sl = dynamic_cast<bb::Slider*>(g.gos[i]->components[c]);
        if (sl) { stamina = sl; break; }
      }
    }
    check("ui-slider-found", stamina != 0 && stamina->fillRect != 0,
          "StaminaSlider must exist with its m_FillRect resolved (PlayerScript writes its value)");
    if (stamina && stamina->fillRect) {
      // The authored range must survive the load. m_MaxValue is 100 for this slider, and
      // PlayerScript writes `stamina / maxStamina * 100f` into it -- so if maxValue silently fell
      // back to the 0..1 default, every value would read as "full" and the bar would look right in
      // School (where the script writes 100) while being wrong everywhere the script does not run.
      check("ui-slider-range-parsed", stamina->minValue == 0.0f && stamina->maxValue == 100.0f,
            "StaminaSlider range is " + f2s(stamina->minValue) + ".." + f2s(stamina->maxValue) +
            ", authored 0..100 (m_MinValue / m_MaxValue)");
      const float saved = stamina->value;
      stamina->value = stamina->minValue + (stamina->maxValue - stamina->minValue) * 0.5f;
      eng::clearDrawList();
      eng::buildDrawList();
      float trackX0 = 0, trackX1 = 0, fillX0 = 0, fillX1 = 0;
      bool haveTrack = false, haveFill = false;
      for (size_t i = 0; i < g.drawList.size(); i++) {
        const eng::DrawQuad& q = g.drawList[i];
        if (q.textured || q.a < 0.99f) continue;
        // Restrict to the stamina bar's own region. ItemSelect is ALSO a pure-red opaque quad and
        // sits at the top RIGHT, so a colour-only filter picks it up and compares the fill against
        // the wrong track -- which is exactly what the first version of this check did.
        if (q.y1 > 60.0f) continue;
        if (q.x0 < (float)g.screenW * 0.3f || q.x1 > (float)g.screenW * 0.7f) continue;
        if (q.r > 0.99f && q.g < 0.01f && q.b < 0.01f) { trackX0 = q.x0; trackX1 = q.x1; haveTrack = true; }
        if (q.g > 0.99f && q.r < 0.01f && q.b < 0.01f) { fillX0 = q.x0; fillX1 = q.x1; haveFill = true; }
      }
      const bool aligned = haveTrack && haveFill && fabsf(fillX0 - trackX0) < 0.5f;
      const bool halved = haveTrack && haveFill &&
                          fabsf((fillX1 - fillX0) - (trackX1 - trackX0) * 0.5f) < 0.5f;
      check("ui-slider-fill-aligned", aligned,
            haveFill ? ("fill starts at x=" + f2s(fillX0) + ", track at x=" + f2s(trackX0) +
                        " -- they must coincide; a mismatch means anchoredPosition was overwritten")
                     : std::string("no green fill quad on the draw list at all"));
      check("ui-slider-fill-proportional", halved,
            haveFill ? ("at 50% the fill is " + f2s(fillX1 - fillX0) + "px of a " +
                        f2s(trackX1 - trackX0) + "px track -- the anchors are not being driven")
                     : std::string("no green fill quad to measure"));
      stamina->value = saved;
      eng::clearDrawList();
      eng::buildDrawList();
    }
  }

  check("ui-drawlist-single-ordering", firstIsGlyph && lastIsGraphic,
        "School's draw list must START with a glyph (NotebookCount, sibling 0) and END with a "
        "graphic (Boots, sibling 8) -- one hierarchy sort over BOTH passes. Graphic-first and "
        "glyph-last means the graphics and label passes were split again");

  check("ui-graphic-rect-is-row-space", slotsFound && slotsTopRight,
        slotsFound ? ("ItemSlots drew at y0=" + f2s(slotsY0) +
                      " -- it is anchored (1,1) = Unity TOP right, so its quad must be in the top "
                      "quarter of the screen; a mirrored graphics pass puts it at the bottom")
                   : std::string("no ItemSlots quad in the School draw list at all"));

  // Every referenced sprite must be BAKED. A backend can only skip a sprite it has no blob for,
  // silently, so without this check a scene edit that adds a sprite -- or a forgotten
  // `node tools\bake-textures.js` -- loses billboards with nothing going red anywhere. The index
  // is read here rather than the .raw files because it is the same lookup the shell does, and it
  // carries the geometry (pixels / PPU around the pivot) that makes a sprite drawable at all.
  bool idxOk = false;
  const std::string sidxText = eng::jsonReadFile("data/sprites-index.json", idxOk);
  eng::JsonValue sidx;
  std::string sidxErr;
  const bool sidxParsed = idxOk && eng::jsonParse(sidxText, sidx, sidxErr);
  check("world-sprites-index-loads", sidxParsed,
        idxOk ? ("data/sprites-index.json did not parse: " + sidxErr)
              : std::string("data/sprites-index.json missing -- run: node tools\\bake-textures.js"));
  if (sidxParsed) {
    int unbaked = 0, noGeom = 0;
    std::string firstMissing;
    for (size_t i = 0; i < sprN; i++) {
      bb::SpriteRenderer* sr = g.spriteRenderers[i];
      if (!sr->sprite || sr->sprite->name.empty()) continue;   // already counted above
      const eng::JsonValue* e = sidx.find(sr->sprite->name.c_str());
      if (!e) {
        unbaked++;
        if (firstMissing.empty()) firstMissing = sr->sprite->name;
        continue;
      }
      // A zero-sized quad is as invisible as a missing one, so the geometry is checked too.
      const eng::JsonValue* uw = e->find("unitsW");
      const eng::JsonValue* uh = e->find("unitsH");
      if (!uw || !uh || uw->asFloat(0.0f) <= 0.0f || uh->asFloat(0.0f) <= 0.0f) noGeom++;
    }
    check("world-sprites-all-baked", unbaked == 0,
          i2s(unbaked) + " referenced sprites are not in data/sprites-index.json (first: " +
          (firstMissing.empty() ? std::string("-") : firstMissing) +
          ") -- a backend can only skip these, silently");
    check("world-sprites-have-size", noGeom == 0,
          i2s(noGeom) + " baked sprites have no usable world size (pixels / spritePixelsToUnits); "
          "a zero-sized quad is as invisible as a missing one");
  }

  // Unity _MainTex TILING. A material's m_Scale multiplies the uvs; WhiteBrickWall is (2,2), so a
  // surface that samples the image once draws its bricks at DOUBLE size. That was the visible
  // difference against a real 1.4.3 screenshot: the walls looked bigger and softer because one
  // stretched copy was drawn where the game tiles 2x2. 7 of the 74 textured materials tile.
  int tiledMats = 0, tiledRenderers = 0;
  for (size_t i = 0; i < n; i++) {
    bb::MeshRenderer* mr = g.meshRenderers[i];
    if (!mr->material || mr->material->mainTexGuid.empty()) continue;
    if (mr->material->texScale.x != 1.0f || mr->material->texScale.y != 1.0f) tiledRenderers++;
  }
  std::set<std::string> seenTiled;
  for (size_t i = 0; i < n; i++) {
    bb::MeshRenderer* mr = g.meshRenderers[i];
    if (!mr->material || mr->material->mainTexGuid.empty()) continue;
    if (mr->material->texScale.x == 1.0f && mr->material->texScale.y == 1.0f) continue;
    seenTiled.insert(mr->material->name);
  }
  tiledMats = (int)seenTiled.size();
  // Material _Color TINT. These materials are not white: 53 of the 74 textured ones are 0.96 grey
  // and 7 are 0.48 (the Dark_ variants). Drawing white made EVERY surface a uniform 4.2% too
  // bright against a real 1.4.3 screenshot -- exactly 1/0.96 -- and the dark ones twice as bright.
  // With the tint applied the walls, floor and ceiling match the reference pixel for pixel.
  int tintedRenderers = 0, darkRenderers = 0;
  for (size_t i = 0; i < n; i++) {
    bb::MeshRenderer* mr = g.meshRenderers[i];
    if (!mr->material || mr->material->mainTexGuid.empty()) continue;
    if (mr->material->colorR < 0.999f) tintedRenderers++;
    if (mr->material->colorR < 0.6f) darkRenderers++;
  }
  check("world-material-tint", tintedRenderers > 1000 && darkRenderers > 0,
        i2s(tintedRenderers) + " renderers carry a non-white _Color (" + i2s(darkRenderers) +
        " of them a dark one); all-white means data/material-textures.json lost _Color and the "
        "whole scene renders ~4% too bright");

  check("world-material-tiling", tiledMats > 0 && tiledRenderers > 100,
        i2s(tiledMats) + " distinct tiled materials across " + i2s(tiledRenderers) +
        " renderers; all-1.0 means data/material-textures.json lost m_Scale and every tiled "
        "surface draws one stretched copy");

  // Unity IMPORT SETTINGS must reach the backend. This game is point-filtered pixel art: 197 of
  // the 205 referenced textures are filterMode 0 (Point) and 204 of 205 have mipmaps OFF.
  // Uploading them bilinear smears a 142x256 Baldi and a 128x128 brick wall, which reads as
  // "lower quality" beside a real screenshot even though the SOURCE PIXELS are identical.
  {
    bool idxOk = false;
    const std::string tidxText = eng::jsonReadFile("data/textures-index.json", idxOk);
    eng::JsonValue tidx;
    std::string tErr;
    const bool parsed = idxOk && eng::jsonParse(tidxText, tidx, tErr);
    int pointCount = 0, mipCount = 0, total = 0, missingFlag = 0;
    if (parsed) {
      for (size_t i = 0; i < tidx.obj.size(); i++) {
        const eng::JsonValue& e = tidx.obj[i].second;
        total++;
        const eng::JsonValue* pt = e.find("point");
        const eng::JsonValue* mp = e.find("mip");
        if (!pt || !mp) { missingFlag++; continue; }
        if (pt->asInt() != 0) pointCount++;
        if (mp->asInt() != 0) mipCount++;
      }
    }
    check("world-texture-import-flags", parsed && missingFlag == 0 && pointCount > total * 9 / 10,
          parsed ? (i2s(pointCount) + "/" + i2s(total) + " textures marked point-filtered, " +
                    i2s(mipCount) + " with mipmaps, " + i2s(missingFlag) +
                    " missing the flags entirely -- the bake must carry filterMode and "
                    "enableMipMap or the backend guesses and the art goes soft")
                 : std::string("data/textures-index.json did not parse"));
  }

  check("world-material-textures", withTex > 1500,
        i2s(withTex) + " MeshRenderers whose material resolves to a baked main texture "
        "(data/material-textures.json; WhiteBrickWall/Ceiling/Carpet/TileFloor dominate)");

  // The registry is scene-local: its nodes' owners die with the scene, so a swap must empty it or
  // a later world pass would walk freed GameObjects.
  eng::unloadScene(true);
  check("world-registry-cleared-on-unload", g.meshRenderers.empty(),
        i2s((long long)g.meshRenderers.size()) + " MeshRenderers left registered after unload");
  // Same lifetime rule as the mesh registry: the Sprite nodes outlive the scene (they are interned
  // assets) but the RENDERERS are scene components, so a stale entry is a freed-pointer walk.
  check("world-sprite-registry-cleared-on-unload", g.spriteRenderers.empty(),
        i2s((long long)g.spriteRenderers.size()) + " SpriteRenderers left registered after unload");
  eng::loadScene("School");
  check("world-registry-refills", g.meshRenderers.size() > 2000,
        i2s((long long)g.meshRenderers.size()) + " MeshRenderers after reloading School");
}

// ---- crash localiser (SIM_TRACE_UPDATE=1) ------------------------------------------------------
// Mirrors runUpdatePhase's component sweep (same order: GameObject registry order, then each
// object's components in attach order, active objects and enabled components only) but names each
// component before calling it. With unbuffered stdout the last line printed is the one that died.
void traceUpdateSweep() {
  eng::State& g = eng::state();
  printf("-- update sweep trace (%d GameObjects) --\n", (int)g.gos.size());
  for (size_t i = 0; i < g.gos.size(); i++) {
    bb::GameObject* go = g.gos[i];
    if (!go->activeSelf) continue;
    for (size_t j = 0; j < go->components.size(); j++) {
      bb::Component* c = go->components[j];
      if (!c->enabled) continue;
      printf("  go[%d] '%s' comp[%d] %s\n", (int)i, go->name.c_str(), (int)j,
             typeid(*c).name());
      c->Update(eng::kFixedDt);
    }
  }
  printf("-- update sweep trace complete --\n");
}


// ---- draw-list dump (SIM_DUMP_DRAWS=<path>) ---------------------------------------------------
// Writes the current draw list as JSON so tools/draw-preview.js can rasterise it into a PNG with
// the baked font atlas. The point is a VISUAL check of the uGUI pipeline: the headless ui-* checks
// pin geometry, colors and glyph metrics numerically, but only a picture shows whether the result
// actually reads as the game's menu. Also the reference the SDL/GL (and iOS GLKit) blit must match.
void dumpDrawList(const char* path) {
  eng::State& g = eng::state();
  FILE* f = fopen(path, "wb");
  if (!f) { printf("dump: cannot open %s\n", path); return; }
  fprintf(f, "{\n  \"scene\": \"%s\",\n", eng::currentSceneName().c_str());
  fprintf(f, "  \"screenW\": %d,\n  \"screenH\": %d,\n", g.screenW, g.screenH);
  fprintf(f, "  \"textures\": [");
  for (size_t ti = 0; ti < g.drawTextures.size(); ti++) {
    fprintf(f, "%s\"%s\"", ti ? ", " : "", g.drawTextures[ti].c_str());
  }
  fprintf(f, "],\n");
  fprintf(f, "  \"quads\": [\n");
  for (size_t i = 0; i < g.drawList.size(); i++) {
    const eng::DrawQuad& q = g.drawList[i];
    fprintf(f, "    {\"x0\":%.3f,\"y0\":%.3f,\"x1\":%.3f,\"y1\":%.3f,"
               "\"r\":%.4f,\"g\":%.4f,\"b\":%.4f,\"a\":%.4f,"
               "\"u0\":%.6f,\"v0\":%.6f,\"u1\":%.6f,\"v1\":%.6f,"
               "\"textured\":%s,\"texId\":%d,\"layer\":%d}%s\n",
            q.x0, q.y0, q.x1, q.y1, q.r, q.g, q.b, q.a, q.u0, q.v0, q.u1, q.v1,
            q.textured ? "true" : "false", q.texId, q.canvasLayer,
            (i + 1 < g.drawList.size()) ? "," : "");
  }
  fprintf(f, "  ]\n}\n");
  fclose(f);
  printf("dump: wrote %d quads to %s\n", (int)g.drawList.size(), path);
}
} // namespace

int main() {
  // Unbuffered: piped stdout is block-buffered, so a crash mid-run would discard every line
  // printed so far -- exactly the output needed to localise the crash.
  setvbuf(stdout, 0, _IONBF, 0);
  eng::state().asyncWaitAtSwap = true;   // async swaps land on their fixed frame (deterministic)

  const char* kPrefsPath = "run/prefs.ini";
  const char* kAssetRoot = "F:/baldidecomp/baldi_s_basics_classic_143_decompile_v5/Assets/";

  // Engine-only checks first: they need no scene and isolate math/RNG/prefs failures from loader
  // failures (a hard scene gate below stops the run, so these must come before it).
  testEulerRoundTrip();
  testEulerUnityDecomposition();
  testRandomDeterminism();

  SIM_MKDIR("run");   // prefsSave() opens the path directly and will not create the directory
  eng::setPrefsPath(kPrefsPath);
  eng::setAssetRoot(kAssetRoot);
  testPlayerPrefs(kPrefsPath);

  // SIM_ONLY_SCENE=<Name> loads just that scene in a fresh process and runs a few frames. Useful
  // when a scene-specific load fault needs isolating from leftover cross-scene state.
  if (const char* only = getenv("SIM_ONLY_SCENE")) {
    printf("-- SIM_ONLY_SCENE=%s --\n", only);
    eng::loadScene(only);
    printf("loaded '%s' (%d GameObjects)\n", eng::currentSceneName().c_str(),
           (int)eng::state().gos.size());
    for (int i = 0; i < 10; i++) { printf("  frame %d\n", i); eng::runFullFrame(1.0f / 50.0f); }
    // ENG_TRACE_DRAW=1: this prints every label's gate result once per scene load
    eng::buildDrawList();
    printf("draw list: %d quads\n", (int)eng::state().drawList.size());
    // SIM_DUMP_FORCE_ACTIVE=1 flips the whole hierarchy active before building. MainMenu bakes
    // every menu GO disabled (the game's own UIManager flow enables them at runtime), so the
    // as-baked scene correctly renders nothing -- same trick ui-drawlist-mainmenu uses.
    if (getenv("SIM_DUMP_FORCE_ACTIVE")) {
      eng::State& st = eng::state();
      for (size_t i = 0; i < st.gos.size(); i++) st.gos[i]->activeSelf = true;
      eng::clearDrawList();
      eng::buildDrawList();
      printf("draw list (forced active): %d quads\n", (int)st.drawList.size());
    }
    if (const char* dump = getenv("SIM_DUMP_DRAWS")) dumpDrawList(dump);
    printf("-- done --\n");
    eng::engineShutdown();
    return 0;
  }

  // ---- hard gate: the School bake must load ----------------------------------------------------
  eng::loadScene("School");
  if (eng::currentSceneName() != "School") {
    printf("FAIL school-load: currentSceneName() == '%s' (bake load failed; run from the project "
           "root so data/scene-School.bake.json resolves)\n", eng::currentSceneName().c_str());
    printf("\nSIM FAILED (1 check)\n");
    return 1;
  }
  pass("school-load");

  testSchoolStructure();
  bb::PlayerScript* player = testScriptBinding();

  // Snapshot the player position, then run 300 frames (6 s at the 50 Hz fixed rate) of the full
  // loop. Nothing drives input, so this exercises Awake/Start fallout, coroutines, the agent
  // integrator, the audio clock and the destroy purge under their own steam.
  bb::Vec3 before = player && player->transform ? player->transform->position : bb::Vec3();
  // SIM_TRACE_FRAMES=1 breaks runFullFrame into its four phases with markers, so a crash during
  // the idle run names the phase it died in instead of just the frame.
  if (getenv("SIM_TRACE_UPDATE")) traceUpdateSweep();
  const bool trace = getenv("SIM_TRACE_FRAMES") != 0;
  for (int i = 0; i < 300; i++) {
    if (!trace) { eng::runFullFrame(1.0f / 50.0f); continue; }
    printf("  [frame %d] begin", i);
    eng::beginFrame(1.0f / 50.0f);
    printf(" fixed");       eng::runFixedSteps();
    printf(" update");      eng::runUpdatePhase();
    printf(" late");        eng::runLateUpdatePhase();
    printf(" end");         eng::endFrame();
    printf(" ok\n");
  }
  pass("school-300-frames");

  check("school-scene-still-loaded", eng::currentSceneName() == "School",
        "scene changed to '" + eng::currentSceneName() + "' during the idle run");

  if (player && player->transform) {
    // No input was injected, so the only motion is gravity/script-driven. A NaN here means the
    // mover or the compose path went non-finite, which is the failure mode that matters.
    bb::Vec3 after = player->transform->position;
    bool finite = after.x == after.x && after.y == after.y && after.z == after.z;
    check("school-player-position-finite", finite,
          "player position went NaN (was " + f2s(before.y) + " y)");
  } else {
    fail("school-player-position-finite", "no player transform to sample");
  }

  testRaycast(player);
  testMeshRaycast();
  testMeshTrigger();
  // Order matters: testTriggerEvents needs fixed steps to run, and testPrefabCloning leaves a
  // MathGame learning game ACTIVE -- GameControllerScript::Update then pins Time::timeScale to 0
  // every frame (how the game freezes the world behind the math keypad), which stops the
  // fixed-step accumulator and with it all trigger dispatch. Triggers first.
  testMeshBlobs();         // baked .asset mesh blobs (no scene needed: they are shared assets)
  testWorldRenderData();   // reloads School at the end, so run it before the scene-swap loop
  testTriggerEvents();
  testAnimatorGraph();
  testVoiceExpiry();
  testPrefabCloning();

  // Real InputManager (InputManager.cs): acts on live bindings via the engine key feed and
  // drives actual player movement. Needs timeScale restored (testPrefabCloning's MathGame pins
  // it to 0, which would freeze the fixed steps this test runs); the test re-pins it itself.
  testInputManager();

  // The audit-style swap proof: empties the scene CARING exactly as processPendingScenes does,
  // so everything after this point runs against the same fresh-swap state the loop produces.
  testSceneSwapSweep();

  // uGUI render pass end-to-end (fonts/anchors/labels/draw list); leaves the fresh-swap state.
  testUiRenderPass();

  // ---- every other scene loads + runs ---------------------------------------------------------
  // School is the big one, but MainMenu is the uGUI-heavy scene (252 script components, almost all
  // of them stock uGUI classes), so it is the real exercise of the class-identification fix. A few
  // frames each is enough to catch null-deref fallout in Start/Update.
  // Asset nodes must keep ONE identity per guid for the whole run, across scene swaps: scripts
  // compare these pointers (DoorScript tests interior materials by identity) and a
  // DontDestroyOnLoad script can carry one across a load. They used to be BindCtx members, i.e.
  // stack locals of the loader, so every load minted fresh nodes and identity silently broke.
  std::string sampleGuid;
  bb::Material* sampleMat = 0;
  if (!eng::state().materials.empty()) {
    sampleGuid = eng::state().materials.begin()->first;
    sampleMat = eng::state().materials.begin()->second;
  }
  check("asset-registry-populated", sampleMat != 0,
        "no Material nodes registered after loading School");

  const char* kScenes[] = { "MainMenu", "Secret", "TestRoom", "GameOver", "Results", "Warning" };
  for (int s = 0; s < 6; s++) {
    std::string name(kScenes[s]);
    // loadScene does NOT unload: the engine's scene-swap path is unloadScene(true) followed by
    // loadScene (see processPendingScenes). keepDontDestroy=true matches it, so DontDestroyOnLoad
    // singletons survive the swap exactly as they do in Unity.
    eng::unloadScene(true);
    eng::loadScene(name);
    if (eng::currentSceneName() != name) {
      fail(("scene-load-" + name).c_str(), "currentSceneName() == '" + eng::currentSceneName() + "'");
      continue;
    }
    for (int i = 0; i < 10; i++) eng::runFullFrame(1.0f / 50.0f);
    pass(("scene-load-" + name).c_str());
  }

  // Six scene swaps later, the same guid must still resolve to the very same node.
  if (sampleMat) {
    std::unordered_map<std::string, bb::Material*>& mats = eng::state().materials;
    std::unordered_map<std::string, bb::Material*>::iterator it = mats.find(sampleGuid);
    check("asset-identity-survives-scene-swaps", it != mats.end() && it->second == sampleMat,
          it == mats.end() ? "the guid vanished from the registry after the scene swaps"
                           : "the guid now maps to a DIFFERENT Material node (identity broken)");
  } else {
    fail("asset-identity-survives-scene-swaps", "no sample Material captured");
  }

  // LOAD SCREEN (user: "a noticeable pause which should be taken up by the loading screen").
  // MainMenu's StoryButton.onClick = StartGame (SceneManager.LoadSceneAsync("School")), then
  // PlayMenu.SetActive(false), LoadScreen.SetActive(true). Unity's async load never lands in the
  // calling frame, so that frame -- with LoadScreen up -- is drawn before the swap.
  {
    eng::unloadScene(true);
    eng::loadScene("MainMenu");
    bb::Button* story = 0;
    bb::GameObject* loadScreen = 0;
    for (size_t i = 0; i < eng::state().gos.size(); i++) {
      bb::GameObject* o = eng::state().gos[i];
      if (o->name == "LoadScreen") loadScreen = o;
      if (o->name == "StoryButton")
        if (bb::Button* b = o->GetComponent<bb::Button>())
          if (b->onClick.listeners.size() >= 3) story = b;   // PlayMenu's (the old one has only StartGame)
    }
    if (story && loadScreen) {
      // its "LOAD" label must lay out on ONE line (TMP wrap rule: no trailing character spacing)
      {
        std::vector<bool> prevA;
        std::vector<bb::GameObject*>& all = eng::state().gos;
        for (size_t i = 0; i < all.size(); i++) prevA.push_back(all[i]->activeSelf);
        for (size_t i = 0; i < all.size(); i++) {
          bool under = false;
          for (bb::Transform* t = all[i]->transform; t; t = t->parent) if (t->gameObject() == loadScreen) under = true;
          all[i]->activeSelf = under;
        }
        eng::clearDrawList();
        eng::buildDrawList();
        float yLo = 1e9f, yHi = -1e9f;
        int glyphs = 0;
        for (size_t i = 0; i < eng::state().drawList.size(); i++) {
          const eng::DrawQuad& q = eng::state().drawList[i];
          if (!q.textured || q.texId >= 0) continue;
          glyphs++;
          yLo = (std::min)(yLo, q.y1); yHi = (std::max)(yHi, q.y1);
        }
        for (size_t i = 0; i < all.size(); i++) all[i]->activeSelf = prevA[i];
        check("load-screen-text-one-line", glyphs == 4 && yHi - yLo < 3.0f,
              i2s(glyphs) + " LOAD glyphs, glyph bottoms spread over " + f2s(yHi - yLo) + " px (two lines when > a line height)");
      }
      story->onClick.Invoke();
      eng::runFullFrame(1.0f / 50.0f);
      const bool drawnWithLoad = eng::currentSceneName() == "MainMenu" && loadScreen->activeSelf;
      eng::runFullFrame(1.0f / 50.0f);
      check("load-screen-shown-before-swap", drawnWithLoad && eng::currentSceneName() == "School",
            std::string("after the Story click's frame: scene ") + (drawnWithLoad ? "MainMenu with LoadScreen up" : "already swapped / no LoadScreen") +
            ", one frame later: " + eng::currentSceneName());
    } else {
      fail("load-screen-shown-before-swap", "StoryButton (3 listeners) or LoadScreen not found in MainMenu");
    }
  }

  // iOS TOUCH UI in the real School (genuine MobileControls, data/prefab-MobileControls.bake.json).
  {
    eng::State& gs = eng::state();
    eng::unloadScene(true);
    gs.mobile = true;
    gs.usingTouch = false;
    eng::touchSetLayout(std::vector<eng::TouchControlDef>());   // build from the scene
    eng::setScreenSize(960, 640);
    eng::loadScene("School");
    bb::GameObject* mc = 0, *simple = 0, *item1Outline = 0, *pauseIcon = 0, *base = 0, *itemIcon = 0;
    for (size_t i = 0; i < gs.gos.size(); i++) {
      bb::GameObject* o = gs.gos[i];
      if (o->name == "MobileControls") mc = o;
      if (o->name == "SimpleTouch") simple = o;
      bb::Transform* p = o->transform ? o->transform->parent : 0;
      const std::string pn = p && p->gameObject() ? p->gameObject()->name : "";
      if (o->name == "Outline" && pn == "Item1 Button") item1Outline = o;
      if (o->name == "RawImage" && pn == "Pause Button") pauseIcon = o;
      if (o->name == "Base" && pn == "Touch Joystick" && simple) base = o;
      if (o->name == "RawImage" && pn == "Item Button") itemIcon = o;
    }
    for (int f = 0; f < 3; f++) eng::runFullFrame(1.0f / 50.0f);
    const bool hiddenBeforeTouch = simple && !simple->activeSelf;
    eng::touchDown(7, 480.0f, 300.0f);              // any touch: InputTypeManager.usingTouch
    for (int f = 0; f < 2; f++) eng::runFullFrame(1.0f / 50.0f);
    eng::touchUp(7);
    eng::runFullFrame(1.0f / 50.0f);
    const bool shownAfterTouch = simple && simple->activeSelf;
    check("ios-touch-ui-instantiated", mc && simple && hiddenBeforeTouch && shownAfterTouch,
          std::string("MobileControls ") + (mc ? "present" : "MISSING") + "; SimpleTouch hidden before a touch: " +
          (hiddenBeforeTouch ? "yes" : "NO") + ", shown after: " + (shownAfterTouch ? "yes" : "NO"));
    // the joystick's live screen rect: 25..175 canvas units at 960/640 = 1.5 (CanvasScaler 640x480
    // match width; the DpCanvasScaler beside it is disabled in the data)
    float jx0 = 0, jy0 = 0, jx1 = 0, jy1 = 0;
    bool haveJ = false;
    for (size_t i = 0; i < gs.gos.size() && !haveJ; i++)
      if (gs.gos[i]->name == "Touch Joystick" && gs.gos[i]->activeInHierarchy())
        haveJ = eng::uiScreenRect(gs.gos[i]->transform, jx0, jy0, jx1, jy1);
    check("ios-touch-joystick-rect", haveJ && fabsf(jx0 - 37.5f) < 0.6f && fabsf(jx1 - 262.5f) < 0.6f &&
          fabsf(jy0 - (640.0f - 262.5f)) < 0.6f && fabsf(jy1 - (640.0f - 37.5f)) < 0.6f,
          "joystick rect (" + f2s(jx0) + "," + f2s(jy0) + ")-(" + f2s(jx1) + "," + f2s(jy1) + "), expected (37.5,377.5)-(262.5,602.5)");
    bb::Image* oimg = item1Outline ? item1Outline->GetComponent<bb::Image>() : 0;
    bb::RawImage* picon = pauseIcon ? pauseIcon->GetComponent<bb::RawImage>() : 0;
    bb::Image* bimg = base ? base->GetComponent<bb::Image>() : 0;
    check("ios-touch-invisible-controls", oimg && !oimg->enabled && picon && picon->enabled && bimg && bimg->enabled,
          std::string("Item1 Outline ") + (oimg ? (oimg->enabled ? "SHOWN (white box)" : "hidden") : "missing") +
          ", pause icon " + (picon ? (picon->enabled ? "shown" : "HIDDEN") : "missing") +
          ", joystick Base " + (bimg ? (bimg->enabled ? "shown" : "HIDDEN") : "missing"));
    // Item button icon: empty slot -> ItemImageScript.noItemSprite (the iOS-only ItemButton texture)
    bb::RawImage* ii = itemIcon ? itemIcon->GetComponent<bb::RawImage>() : 0;
    check("ios-item-button-icon", ii && ii->texture && ii->texture->name == "ios_sharedassets2_assets_465",
          std::string("Item button icon texture: ") + (ii && ii->texture ? ii->texture->name : std::string("none")));
    // a real touch on the Run button (bottom-right row: x -125 from the right, 75x75, pivot (1,0))
    const float rx = 960.0f - (125.0f + 37.5f) * 1.5f, ry = 640.0f - (25.0f + 37.5f) * 1.5f;
    // Run Button is a ToggleSwitch (_buttonType 1): a tap latches Run on through the release, the
    // next tap turns it off.
    eng::touchDown(8, rx, ry);
    eng::runFullFrame(1.0f / 50.0f);
    eng::touchUp(8);
    eng::runFullFrame(1.0f / 50.0f);
    const bool runHeld = bb::ReInputPlayer().GetButton("Run");
    eng::touchDown(8, rx, ry);
    eng::runFullFrame(1.0f / 50.0f);
    eng::touchUp(8);
    eng::runFullFrame(1.0f / 50.0f);
    // COLOUR TINT (genuine _transitionColorTint): idle buttons at alpha 0.235 x their own colour,
    // pressed fades to 0.89 within the 0.1 s fade, released returns to 0.235.
    {
      bb::Image* fill = 0;
      for (size_t i = 0; i < gs.gos.size(); i++) {
        bb::GameObject* o = gs.gos[i];
        bb::Transform* p = o->transform ? o->transform->parent : 0;
        if (o->name == "Fill" && p && p->gameObject() && p->gameObject()->name == "Interact Button" && o->activeInHierarchy())
          fill = o->GetComponent<bb::Image>();
      }
      const float idle = fill ? fill->tint.a : -1.0f;
      const float ix = 960.0f - (25.0f + 37.5f) * 1.5f, iy = 640.0f - (25.0f + 37.5f) * 1.5f;
      eng::touchDown(9, ix, iy);
      for (int f = 0; f < 6; f++) eng::runFullFrame(1.0f / 50.0f);   // > 0.1 s
      const float pressed = fill ? fill->tint.a : -1.0f;
      eng::touchUp(9);
      for (int f = 0; f < 6; f++) eng::runFullFrame(1.0f / 50.0f);
      const float back = fill ? fill->tint.a : -1.0f;
      check("ios-touch-button-tint", fill && fabsf(idle - 0.23529f) < 1e-3f && fabsf(pressed - 0.89020f) < 1e-3f &&
            fabsf(back - 0.23529f) < 1e-3f,
            "Interact Fill tint alpha idle " + f2s(idle) + " (0.235), pressed " + f2s(pressed) + " (0.890), released " + f2s(back));
    }
    check("ios-touch-run-toggle", runHeld && !bb::ReInputPlayer().GetButton("Run"),
          std::string("tap at (") + f2s(rx) + "," + f2s(ry) + ") -> Run " + (runHeld ? "on after release" : "NOT on") +
          ", second tap -> " + (bb::ReInputPlayer().GetButton("Run") ? "STILL ON" : "off"));
    // The genuine iOS scripts read the other buttons through Rewired too (ios-ref/classes/*.ios.c):
    // GameControllerScript Pause/Item1-3, CameraScript LookBack, DoorScript (and Notebook/Pickup/
    // EndlessNotebook) Interact. Each is pressed here by a real touch on the live control's centre.
    {
      struct Tap {
        static bool centre(const char* name, float& x, float& y) {
          eng::State& g = eng::state();
          for (size_t i = 0; i < g.gos.size(); i++)
            if (g.gos[i]->name == name && g.gos[i]->activeInHierarchy()) {
              float x0, y0, x1, y1;
              if (!eng::uiScreenRect(g.gos[i]->transform, x0, y0, x1, y1)) continue;
              x = (x0 + x1) * 0.5f; y = (y0 + y1) * 0.5f;
              return true;
            }
          return false;
        }
        static bool tap(const char* name, int id) {
          float x, y;
          if (!centre(name, x, y)) return false;
          eng::touchDown(id, x, y);
          eng::runFullFrame(1.0f / 50.0f);
          eng::touchUp(id);
          eng::runFullFrame(1.0f / 50.0f);
          return true;
        }
      };
      bb::GameControllerScript* gc = 0;
      bb::CameraScript* cs = 0;
      bb::PlayerScript* ps = 0;
      std::vector<bb::DoorScript*> doors;
      for (size_t i = 0; i < gs.gos.size(); i++)
        for (size_t k = 0; k < gs.gos[i]->components.size(); k++) {
          bb::Component* c = gs.gos[i]->components[k];
          if (bb::GameControllerScript* x = dynamic_cast<bb::GameControllerScript*>(c)) gc = x;
          if (bb::CameraScript* x = dynamic_cast<bb::CameraScript*>(c)) cs = x;
          if (bb::PlayerScript* x = dynamic_cast<bb::PlayerScript*>(c)) ps = x;
          if (bb::DoorScript* x = dynamic_cast<bb::DoorScript*>(c)) doors.push_back(x);
        }
      // Item2 / Item3 / Item1 slot buttons (invisible, over the item slots) -> itemSelected
      const bool t2 = Tap::tap("Item2 Button", 20); const int sel2 = gc ? gc->itemSelected : -1;
      const bool t3 = Tap::tap("Item3 Button", 21); const int sel3 = gc ? gc->itemSelected : -1;
      const bool t1 = Tap::tap("Item1 Button", 22); const int sel1 = gc ? gc->itemSelected : -1;
      check("ios-touch-item-slots", gc && t1 && t2 && t3 && sel2 == 1 && sel3 == 2 && sel1 == 0,
            "taps Item2/Item3/Item1 -> itemSelected " + i2s(sel2) + "/" + i2s(sel3) + "/" + i2s(sel1) + " (1/2/0)");
      // Pause button: one tap pauses, the next unpauses
      const bool tp = Tap::tap("Pause Button", 23);
      const bool pausedNow = gc && gc->gamePaused && bb::Time::timeScale == 0.0f;
      Tap::tap("Pause Button", 24);
      const bool resumed = gc && !gc->gamePaused && bb::Time::timeScale == 1.0f;
      check("ios-touch-pause-button", tp && pausedNow && resumed,
            std::string("tap -> ") + (pausedNow ? "paused" : "NOT paused") + ", tap again -> " + (resumed ? "resumed" : "NOT resumed"));
      // Look Button held -> CameraScript.lookBehind 180, released -> 0
      float lx = 0, ly = 0;
      const bool hl = Tap::centre("Look Button", lx, ly);
      eng::touchDown(25, lx, ly);
      eng::runFullFrame(1.0f / 50.0f);
      const int lbHeld = cs ? cs->lookBehind : -1;
      eng::touchUp(25);
      eng::runFullFrame(1.0f / 50.0f);
      const int lbFree = cs ? cs->lookBehind : -1;
      check("ios-touch-look-back", hl && lbHeld == 180 && lbFree == 0,
            "Look Button held -> lookBehind " + i2s(lbHeld) + " (180), released -> " + i2s(lbFree) + " (0)");
      // Interact Button with the player facing a closed door 4 units away -> the door opens
      bool opened = false;
      std::string why = doors.empty() ? "no doors" : "";
      if (ps && ps->transform && !doors.empty() && doors[0]->trigger && doors[0]->trigger->transform) {
        bb::DoorScript* d = doors[0];
        bb::Transform* pt = ps->transform;
        const bb::Vec3 home = pt->position;
        const bb::Quat homeRot = pt->rotation;
        const bb::Transform* tt = d->trigger->transform;
        bb::Vec3 dir = bb::normalized(tt->rotation.rotate(bb::Vec3(0.0f, 0.0f, 1.0f)));
        dir.y = 0.0f;
        dir = bb::normalized(dir);
        bb::Vec3 at = tt->position - dir * 4.0f;
        at.y = home.y;
        pt->position = at;
        const float yaw = atan2f(dir.x, dir.z);
        pt->rotation = bb::Quat(0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f));
        ps->playerRotation = pt->rotation;
        for (int f = 0; f < 3; f++) eng::runFullFrame(0.02f);
        const bool wasOpen = d->bDoorOpen;
        const bool ti = Tap::tap("Interact Button", 26);
        opened = ti && !wasOpen && d->bDoorOpen;
        if (!opened) why = std::string("tap ") + (ti ? "ok" : "MISSING") + ", open before " + (wasOpen ? "yes" : "no");
        pt->position = home;
        pt->rotation = homeRot;
        ps->playerRotation = homeRot;
      }
      check("ios-touch-interact-opens-door", opened, opened ? "Interact Button opened the faced door" : why);
      // Joystick stick graphic: anchoredPosition = original + value * 60 (Rewired TouchJoystick)
      bb::RewiredTouchControl* joy = 0;
      for (size_t i = 0; i < gs.gos.size() && !joy; i++)
        if (gs.gos[i]->name == "Touch Joystick" && gs.gos[i]->activeInHierarchy())
          joy = gs.gos[i]->GetComponent<bb::RewiredTouchControl>();
      const float jcx = (jx0 + jx1) * 0.5f, jcy = (jy0 + jy1) * 0.5f;
      eng::touchDown(27, jcx + 45.0f, jcy);           // half of the 90 px (60 units x 1.5) range, right
      eng::runFullFrame(1.0f / 50.0f);
      const bb::Vec2 half = joy && joy->stick ? joy->stick->anchoredPosition : bb::Vec2(-999, -999);
      eng::touchMove(27, jcx, jcy - 300.0f);          // far above: clamped to the full range, up
      eng::runFullFrame(1.0f / 50.0f);
      const bb::Vec2 full = joy && joy->stick ? joy->stick->anchoredPosition : bb::Vec2(-999, -999);
      eng::touchUp(27);
      eng::runFullFrame(1.0f / 50.0f);
      const bb::Vec2 rest = joy && joy->stick ? joy->stick->anchoredPosition : bb::Vec2(-999, -999);
      // ThinkPad open (iOS MathGame canvas sortingOrder 10): its full-screen backdrop is drawn over
      // the touch controls and takes their touches -- Interact / Run taps do nothing under it.
      {
        bb::GameObject* mg = eng::instantiatePrefab("MathGame", bb::Vec3(), bb::Quat::identity());
        bb::Canvas* mcv = mg ? mg->GetComponent<bb::Canvas>() : 0;
        if (bb::MathGameScript* ms = mg ? mg->GetComponent<bb::MathGameScript>() : 0) {   // as NotebookScript does
          ms->gc = gc;
          ms->baldiScript = gc ? gc->baldiScrpt : 0;
          ms->playerPosition = ps && ps->transform ? ps->transform->position : bb::Vec3();
        }
        eng::runFullFrame(1.0f / 50.0f);                // MathGameScript.Start -> gc.ActivateLearningGame
        int joyOrder = -1, bgOrder = -1;
        for (size_t i = 0; i < gs.gos.size(); i++) {
          if (gs.gos[i]->name == "Base" && gs.gos[i]->activeInHierarchy()) eng::uiCanvasOrder(gs.gos[i]->transform, joyOrder);
          if (gs.gos[i]->name == "YCTPBG" && gs.gos[i]->activeInHierarchy()) eng::uiCanvasOrder(gs.gos[i]->transform, bgOrder);
        }
        float ix = 0, iy = 0, rbx = 0, rby = 0;
        Tap::centre("Interact Button", ix, iy);
        Tap::centre("Run Button", rbx, rby);
        eng::touchDown(28, ix, iy);
        eng::runFullFrame(1.0f / 50.0f);
        const bool interactUnder = bb::ReInputPlayer().GetButton("Interact");
        eng::touchUp(28);
        Tap::tap("Run Button", 29);
        const bool runUnder = bb::ReInputPlayer().GetButton("Run");
        const int mgOrder = mcv ? mcv->sortingOrder : -1;   // read before the pad is destroyed
        if (gc) gc->DeactivateLearningGame(mg);
        else if (mg) bb::Destroy(mg);
        eng::runFullFrame(1.0f / 50.0f);
        eng::runFullFrame(1.0f / 50.0f);
        eng::touchDown(30, ix, iy);
        eng::runFullFrame(1.0f / 50.0f);
        const bool interactAfter = bb::ReInputPlayer().GetButton("Interact");
        eng::touchUp(30);
        eng::runFullFrame(1.0f / 50.0f);
        check("ios-thinkpad-covers-touch-controls", mgOrder == 10 && bgOrder == 10 && joyOrder == 1 &&
              !interactUnder && !runUnder && interactAfter,
              "MathGame canvas order " + i2s(mgOrder) + " (10), backdrop " + i2s(bgOrder) +
              " over joystick " + i2s(joyOrder) + "; under the pad Interact " + (interactUnder ? "FIRED" : "blocked") +
              ", Run tap " + (runUnder ? "TOGGLED" : "blocked") + "; pad closed -> Interact " + (interactAfter ? "works" : "BROKEN"));
      }
      check("ios-touch-stick-follows-finger", joy && joy->stick &&
            fabsf(half.x - 30.0f) < 0.1f && fabsf(half.y) < 0.1f && fabsf(full.x) < 0.1f && fabsf(full.y - 60.0f) < 0.1f &&
            fabsf(rest.x) < 1e-4f && fabsf(rest.y) < 1e-4f,
            "stick at (" + f2s(half.x) + "," + f2s(half.y) + ") half-right (30,0), (" + f2s(full.x) + "," + f2s(full.y) +
            ") full-up (0,60), released (" + f2s(rest.x) + "," + f2s(rest.y) + ") (0,0)");
    }
    eng::unloadScene(true);
    gs.mobile = false;
    gs.usingTouch = false;
    eng::setScreenSize(1280, 720);
    eng::loadScene("School");
  }

  testFirstPrizePin();     // reloads School (PC mode)
  testUserReport11r();     // reloads School (PC mode)
  if (const char* gw = getenv("SIM_GAMEOVER_WAV")) dumpGameOverWav(gw);
  if (getenv("SIM_VOICES")) dumpVoiceCount();
  if (getenv("SIM_AGENTS")) dumpAgentMotion();
  if (const char* gl = getenv("SIM_GAMEOVER_LIVE")) dumpGameOverLive(gl);
  testSceneSwapAudioHold();
  testJumpscareCamera();
  testGameOverVoiceRule();
  testLoadScreenSpinsDuringLoad();
  testRenderListsAfterDestroy();
  testTurnSliderDrag();
  testBroadphaseMatchesBrute();
  testDdolStillAlive();

  eng::engineShutdown();
  pass("engine-shutdown");

  if (gFails) { printf("\nSIM FAILED (%d check%s)\n", gFails, gFails == 1 ? "" : "s"); return 1; }
  printf("\nSIM OK - all checks passed\n");
  return 0;
}
