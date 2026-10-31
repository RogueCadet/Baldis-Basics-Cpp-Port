// eng-internal.h -- engine internals shared between eng-core.cpp, the scene (bake) loader and
// the headless sim / testbed shell. NOT part of the translated-script surface.
#pragma once
#include "eng.h"
#include "eng-json.h"
#include <string>
#include <vector>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>
#include <atomic>
#include <mutex>

namespace eng {

// UiText-derived TMP label the State maps hold raw pointers to (full type near the loader seams)
struct EngineUiText;

const float kFixedDt = 1.0f / 50.0f;   // Unity FixedUpdate default (charter parity rate)
// --profile: per-phase seconds since the last read [fixed, agents, triggers, update, late, end]
void readFrameProfile(double out[6]);
// Unity's transform hierarchy at runtime: children follow their parent's moves, localPosition
// writes take effect. Run after each phase that can move things (eng-core.cpp).
void propagateHierarchy();
// App lifecycle (iOS: applicationWillResignActive -> true, applicationDidBecomeActive -> false):
// OnApplicationPause to every component on an active GameObject, as Unity broadcasts it.
void applicationPause(bool pauseStatus);

// ---- iOS touch controls -> Rewired TouchControl elements (eng-rewired.cpp) ----------------------
// One control per on-screen Rewired component, in SCREEN pixels (rows down), in uGUI sibling
// order (later = on top). Blocker = a raycast target that absorbs touches (Touch Pad_Dum).
struct TouchControlDef {
  enum Kind { Joystick, Pad, Button, Blocker };
  Kind kind = Blocker;
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  int elemX = -1, elemY = -1, elemButton = -1;   // TouchControl element ids (data/rewired-touch.json)
  float rangePx = 0.0f;                          // joystick: _stickRange in screen pixels
  const void* owner = 0;                         // the scene component (keeps a touch on its control)
};
void touchSetLayout(const std::vector<TouchControlDef>& controls);
void touchSetDpi(float dpi);                     // Screen.dpi (the TouchPad's Physical format)
void touchDown(int id, float x, float y);
void touchMove(int id, float x, float y);
void touchUp(int id);
void rewiredBeginFrame();                        // controls -> elements -> actions, once per frame
// Screen rect (pixels, rows down) of a UI transform under an active canvas: the same chain, canvas
// scale and layout maths the hit test and the draw list use. False when not on an active canvas.
bool uiScreenRect(const bb::Transform* t, float& x0, float& y0, float& x1, float& y1);
bool uiCanvasOrder(const bb::Transform* t, int& order);
void setBroadphaseBrute(bool brute);   // true: every collider is a candidate (the pre-broadphase path)
void loadNavMesh(const std::string& scene);   // data/navmesh-<scene>.json -> State::navPolys
void startAsyncSceneParse(const std::string& name);   // LoadSceneAsync worker (eng-scene.cpp)
void loadPump();                              // State::loadPump, if set (long-load checkpoints)
void sceneShown();
// RAII guard for State::audioMutex (main-thread scopes that run game code outside runFullFrame)
struct AudioLock {
  AudioLock();
  ~AudioLock();
};                            // the swapped-in scene's first frame is on screen
// shell --facing: player yaw through PlayerScript.playerRotation as well (game-actions.cpp)
bool setPlayerFacing(float yawDeg);
bool debugBaldiCatch();                       // shell --catch: Baldi catches the player now
bool debugSpoopMode();                        // shell --spoop: wake every NPC (profiling)
// Unity Selectable.DoStateTransition for Normal (false) / Highlighted (true).
void applySelectableState(bb::Selectable* s, bool highlighted);

// ---- engine state (methods in eng-core.cpp; members shared with the scene loader/sim) --------
struct DoomEntry {
  bb::GameObject* go;
  float t;
  DoomEntry(bb::GameObject* g_, float d_) : go(g_), t(d_) {}
};

// The scene's GENUINE navmesh (tools/bake-navmesh.js decodes Scene/<Scene>/NavMesh.asset): convex
// polygons in world space, linked through shared-edge portals. Agents path over it.
struct NavLink { int poly = -1; float ax = 0, az = 0, bx = 0, bz = 0; };
struct NavPoly { std::vector<bb::Vec3> v; std::vector<NavLink> links; bb::Vec3 centre; };

struct AgentSim {
  bb::Vec3 dest;
  bool hasDest = false;
  bb::Vec3 pendingVel;      // setVelocity injection (BsodaEffect push): REPLACES the path motion
  bool hasPendingVel = false;
  int pendingFrame = 0;     // Time.frameCount when set; live for the fixed steps up to the next Update
  bb::Vec3 lastStepVel;     // actual per-second motion the last fixed step produced
  // navmesh path (eng::agentRepath): the corner list the agent walks, and where it was planned to
  std::vector<bb::Vec3> corners;
  size_t corner = 0;
  float curSpeed = 0.0f;   // actual speed, ramping toward agent.speed at agent.acceleration
  bb::Vec3 plannedFor;
  bool planned = false;
};

struct AsyncSceneLoad;   // eng-scene.cpp: the LoadSceneAsync worker (thread + parsed bake)

struct AudioVoice { bb::AudioClip* clip = 0; float t = 0.0f;
  AudioVoice(bb::AudioClip* c_ = 0, float t_ = 0.0f) : clip(c_), t(t_) {}
  double startSamples = 0.0; // output-mix stamp: mixClock when PlayOneShot fired
  float gain = 1.0f;         // PlayOneShot volumeScale (composed with src->volume at mix time)
};

struct AudioSim {
  bool playing = false;     // main voice state (loop-aware clock)
  bool paused = false;      // AudioSource.Pause (TapePlayer timeScale==0 semantics go through here)
  float elapsed = 0.0f;
  std::vector<AudioVoice> shots; // PlayOneShot voices (each expiry = clip length / pitch)
  // Output-mix plumbing (separate from the script-logic clocks above): main voice start position
  // on the mixer's sample timeline, -1 = never started (silence). Pause stamps the current mix
  // position; UnPause shifts all starts by the pause span so voices resume where they stopped,
  // exactly Unity's semantics.
  double mainStart = -1.0;
  double pauseStamp = 0.0;
};

struct MoveSim {            // CharacterController motion ledger (velocity() parity)
  bb::Vec3 lastMotion;
  float lastDt = 0.0f;
};

// ---- TMP fonts (font-bake.js output) ------------------------------------------
// Data/font-<Name>.json + optional data/font-<Name>.atlas raw RBGA8 blob. The bake resolved the
// font asset's own glyph-rect y convention against actual pixels (pixel-proven at build time),
// so glyphs carry FINAL atlas UVs: v0/v1 are ROW fractions of the blob (0 = first row = image
// top). Fonts without shipped atlas pixels (metrics-only bakes: COMIC, LiberationSans SDF, ...)
// give correct advances/line metrics but no ink; the draw pass renders them advance-only.
// Registry lifetime: persistent, like materials/sprites/clips -- unloadScene never touches
// fonts, engineShutdown frees everything naturally (value maps, no manual nodes).
struct TMPFontGlyph {
  float u0 = 0, v0 = 0, u1 = 0, v1 = 0;  // atlas UVs, engine-ready
  float w = 0, h = 0;                    // glyph rect in atlas pixels (quad size * pointScale)
  float xo = 0, yo = 0;                  // pen offsets: xo = left bearing, yo = top above baseline
  float adv = 0;                         // pen advance (baked point-size units)
};

struct TMPFontData {
  std::string name;
  bool sdf = false;          // atlas alpha is a signed distance field (alpha-test at 0.5)
  float pointSize = 24.0f;   // face metrics, all in point-size units
  float lineHeight = 33.0f;  // ascender + |descender| == lineHeight holds for the shipped faces
  float ascender = 26.0f;
  float descender = -7.0f;   // NEGATIVE below baseline
  float scale = 1.0f;
  int atlasW = 0, atlasH = 0;
  std::vector<unsigned char> rgba;        // empty = metrics-only font (no ink exists)
  std::map<int, TMPFontGlyph> glyphs;     // unicode codepoint -> record (id IS the codepoint)

  const TMPFontGlyph* glyphFor(int codepoint) const {
    std::map<int, TMPFontGlyph>::const_iterator it = glyphs.find(codepoint);
    return it == glyphs.end() ? 0 : &it->second;
  }
  float pointScaleFor(float fontSize) const {
    const float s = scale > 0.0f ? scale : 1.0f;
    return pointSize > 0.0f ? fontSize / (pointSize * s) : 1.0f;
  }
};

// ---- uGUI rect rects live ON the components -------------------------------------
// RectTransform carries anchorMin/Max, sizeDelta, pivot, and anchoredPosition directly (Unity
// shape: the GO's transform IS its rect, so no side registry and no extra purge points).

// ---- uGUI draw list (headless render pass capture) -------------------------------
// buildDrawList() converts active Canvas-overlay TMP labels into screen-space quads with
// atlas UVs. Coordinates: x right, y DOWN, origin top-left (row space, matches the atlas blob
// orientation). Unity's RectTransform layout math is reproduced exactly (anchor reference =
// lerp(anchorMinPoint, anchorMaxPoint, pivot); size = anchor span + sizeDelta; pivot offset);
// text layout uses the font-bake face metrics with TMP's baseline rule
// (glyph top row = baseline - yOffset*pointScale, verified against atlas ink). A host (SDL/GL
// backend, iOS GLKit, or the sim) owns the list: clear + build + read per rendered frame.
struct DrawQuad {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // screen rect; y row space (top-left origin)
  float r = 0, g = 0, b = 0, a = 0;
  float u0 = 0, v0 = 0, u1 = 0, v1 = 0;  // atlas UVs (textured quads)
  int canvasLayer = 0;                   // Canvas sorting (reserved for multi-canvas ordering)
  bool textured = true;                  // true = sample a texture/atlas, false = solid fill
  // Which image this quad samples. -1 = the FONT ATLAS (glyph quads); >= 0 indexes
  // State::drawTextures, the per-build table of uGUI texture ids. A backend binds the
  // corresponding texture before drawing the quad; the sim/preview resolves it the same way.
  int texId = -1;
  // Glyph quads: WHICH font's atlas (texId < 0). There used to be one atlas for everything
  // (COMIC_Pro), so every other font's glyphs sampled COMIC_Pro's page with their own UVs.
  const struct TMPFontData* font = 0;
};

// A baked .asset mesh (data/mesh/<guid>.mesh, produced by tools\bake-meshes.js). These are the
// desks / chairs / lockers / BSODA sides -- the only geometry in the game that is NOT a Unity
// built-in primitive. Every backend draws from this same flat data; see eng-mesh.cpp.
struct MeshAsset {
  std::string guid;
  std::vector<float> verts;              // 5 floats per vertex: x y z u v (Unity space, Unity uv)
  std::vector<unsigned short> indices;   // triangle list, every index < verts.size()/5
  float center[3];                       // localAABB from the asset, for culling / sanity checks
  float extent[3];
  MeshAsset() {
    for (int i = 0; i < 3; i++) { center[i] = 0.0f; extent[i] = 0.0f; }
  }
  size_t vertCount() const { return verts.size() / 5; }
  size_t triCount() const { return indices.size() / 3; }
};

// A six-sided RenderSettings skybox (data/skybox.json, produced by
// tools\bake-material-textures.js from the material's _FrontTex/_BackTex/../_DownTex slots).
// School and Secret configure Skybox.mat (legacy 6-sided shader); every other scene points
// RenderSettings.m_SkyboxMaterial at Unity's built-in default material, which bake.js DROPS --
// so an empty currentSceneSkybox() means "no skybox, keep the clear colour", which is Unity's
// behaviour for those scenes exactly.
struct SkyboxData {
  std::string matGuid;
  std::string faces[6];        // front, back, left, right, up, down  (TEXTURE guids)
  float tintR = 0.5019608f;    // _Tint, multiplied into every face (legacy 6-sided shader)
  float tintG = 0.5019608f;
  float tintB = 0.5019608f;
  float rotation = 0.0f;       // _Rotation: degrees about Y (Skybox.mat authors 129)
  float exposure = 1.0f;       // _Exposure (all scenes author 1; kept for completeness)
};

// A loader-created onClick listener: Unity's inspector-wired SetActive(bool). Engine-owned,
// because unlike the script adapters (which live with their owning script) nothing else holds it.
struct SetActiveAction : public bb::UnityAction {
  bb::GameObject* target;
  bool value;
  SetActiveAction(bb::GameObject* t, bool v) : target(t), value(v) {}
  void Invoke() override { if (target) target->SetActive(value); }
};

struct State {
  bool playing = true;
  bool quitting = false;
  float dtUnscaled = 0.0f;
  float dtScaled = 0.0f;
  float dtContext = kFixedDt;  // what Time.deltaTime() returns right now
  float acc = 0.0f;            // fixed-step accumulator
  float doomClock = 0.0f;      // Unity Time.time (scaled; advances in fixed steps)
  int screenW = 1280, screenH = 720;
  bool queriesHitTriggers = true;   // Unity Physics.queriesHitTriggers default
  bool meshWarned = false;          // one-time mesh-collider raycast skip notice

  bb::Color ambientLight;           // RenderSettings.ambientLight store
  bb::Material* skybox = 0;

  // ---- -debug cheat menu (GameControllerScript drives it; see bb::debugUiFrame) ----------
  bool debugUiOpen = false;         // panel expanded (the game toggles this with the / key)
  bool cheatNpcIgnore = false;      // "make NPCs ignore you": npc scripts early-return on it
  bb::DebugUiSink* debugUiSink = 0; // set each frame by the game; null = nothing to draw
  float uiPointerX = -1.0f, uiPointerY = -1.0f;   // last uiPointerMove window coords
  // Row hit rects from the PREVIOUS build (clicks are hit-tested one frame late -- labels are
  // static per row so the lag never shows).
  static const int kDebugUiMaxRows = 8;
  float dbgRowX[kDebugUiMaxRows], dbgRowY[kDebugUiMaxRows], dbgRowW[kDebugUiMaxRows], dbgRowH[kDebugUiMaxRows];
  int dbgRowCount = 0;
  int dbgHoverRow = -1;             // pointer row for the highlight + hover cursor
  bool debugEspOn = false;          // "ESP through walls": marked sprites redrawn depth-test-off
  std::vector<std::string> debugEspNames; // the GO names (root or any ancestor) the esp pass marks

  // registries
  std::vector<bb::GameObject*> gos;
  std::vector<bb::BoxCollider*> boxColliders;
  std::vector<bb::CapsuleCollider*> capsuleColliders;
  std::vector<bb::MeshCollider*> meshColliders;    // raycast-skipped until Phase 3 mesh bounds
  std::vector<bb::NavMeshAgent*> agents;
  std::vector<bb::AudioSource*> audioSources;
  std::vector<bb::Camera*> cameras;
  std::vector<bb::Rigidbody*> rigidbodies;         // integrateRigidbodies (dynamic ones move)
  // Every MeshRenderer in the live scene, in load order. MeshRenderer is Object-derived, so
  // it is NOT reachable through go->components -- the world render pass needs this registry
  // (and the owner pointers on the node) to find anything to draw.
  std::vector<bb::MeshRenderer*> meshRenderers;
  // Billboard pass. SpriteRenderer IS a Component (unlike MeshRenderer) so it is reachable
  // through go->components, but a backend would have to walk 5207 objects every frame to find
  // 151 of them; the registry is the same shortcut, and is cleared on unload for the same
  // reason -- the nodes outlive the scene but their gameObject/transform do not.
  std::vector<bb::SpriteRenderer*> spriteRenderers;
  std::vector<bb::Transform*> transforms;          // lifetime registry (GO owns the transform)
  std::vector<bb::Object*> ownedObjects;           // engine-allocated nodes (clips, UiText)
  std::unordered_map<bb::Transform*, std::vector<bb::Transform*> > childOf;

  std::vector<DoomEntry> doomed;

  // scene manager
  std::string curScene;
  std::string pendingScene;
  bool hasPendingScene = false;
  // iOS build behaviour (touch controls, Rewired action reads, iOS-only script paths). Set by the
  // iOS shell, and by the Windows testbed's --ios so those paths run and are tested on Windows.
  bool mobile = false;
  float screenDpi = 0.0f;   // Screen.dpi (0 = unknown, as Unity reports when it cannot tell)
  bool usingTouch = false;  // InputTypeManager.usingTouch
  // SceneManager.LoadSceneAsync: the swap lands only after this many more end-of-frames, so the
  // frame the request was made in is rendered first (Unity's async load never completes in the
  // calling frame). MainMenu's Story/Endless buttons call StartGame and THEN SetActive(LoadScreen):
  // with a same-frame swap the load screen was never drawn and the load looked like a freeze.
  std::string asyncScene;
  int asyncFramesLeft = 0;
  int fixedStepsLastFrame = 0;   // runFixedSteps' step count (perf log: catch-up steps on slow devices)
  size_t colliderVersion = 0;    // bumped when a collider is added/removed (broadphase rebuild)
  // PHASE DISPATCH LISTS: the components that implement FixedUpdate / Update / LateUpdate (noOp
  // bit clear), in g.gos then go->components order -- the order the full walk used. Rebuilt when
  // a GameObject or component is added or removed (componentVersion) or a component newly marks
  // itself a no-op; ~10k School components shrink to a few dozen per phase.
  size_t componentVersion = 0;
  // BACKGROUND READ + PARSE. Unity's LoadSceneAsync loads while the current scene keeps updating
  // and drawing -- the MainMenu LoadScreen's Baldi head (LAOD: an unscaled-time Animator stepping
  // HeadSpin0000..) spins all through the load. A worker thread reads and parses the scene bake
  // (pure data, no engine state); the swap waits until it is done, then builds from the parsed
  // document, so the main thread only blocks for the build. asyncWaitAtSwap (capture runs, sim):
  // the swap lands exactly asyncFramesLeft frames after the request, waiting for the worker
  // there, so deterministic runs are unchanged.
  AsyncSceneLoad* asyncLoad = 0;
  bool asyncWaitAtSwap = false;

  // UI/event system
  bb::EventSystem* esCurrent = 0;
  bb::GameObject* selectedGO = 0;
  bb::Button* hoveredButton = 0;
  bb::Slider* dragSlider = 0;       // slider captured by a pointer press (uiSliderDown)
  int dragPointer = -1;   // last uiPointerMove hit (enter/exit edges dispatch from here)
  bb::Camera* camMainCache = 0;
  int camMainCachedAtFrame = -1;

  // Asset-node registries: exactly one identity node per guid for the whole run, deliberately
  // OUTLIVING any single scene load (see the note in BindCtx). Freed by engineShutdown via
  // ownedObjects. Prefab templates are inert named GameObjects registered with noteRuntimeObject
  // only, so GameObject::Find can never see a template.
  std::unordered_map<std::string, bb::GameObject*> prefabTemplates; // prefab guid -> inert shell
  std::unordered_map<std::string, bb::Material*> materials;         // guid -> node (identity tests)
  std::unordered_map<std::string, bb::Sprite*> sprites;
  std::unordered_map<std::string, bb::Texture*> textures;

  // audio asset registry (guid -> node, lazy WAV duration)
  std::string assetRoot = "F:/baldidecomp/baldi_s_basics_classic_143_decompile_v5/Assets/";
  std::unordered_map<std::string, bb::AudioClip*> clipByGuid;
  std::unordered_map<bb::AudioClip*, std::string> clipPath;
  std::unordered_map<bb::AudioClip*, float> clipLen;
  std::unordered_map<bb::AudioSource*, AudioSim> audioSim;
  // Listener position for the 3D audio roll-off, cached on the MAIN thread each frame (from the
  // main camera) right before refreshAudioSpatial snapshots the sources. listenerValid becomes 1
  // only after the first successful cache, so the mix thread never reads a garbage first frame;
  // between-frame float staleness is benign (positions move slowly vs 46 ms audio blocks).
  std::atomic<int> listenerValid{0};
  bb::Vec3 listenerPos;
  // Output mixer timeline: advanced ONLY by eng::audioPull (called from the shell's waveOut feed
  // thread), so Play()/PlayOneShot() stamps read it atomically while the feed is live.
  std::atomic<long long> mixClock{0}; // output samples emitted so far (44100 Hz stereo)
  // SCENE-SWAP AUDIO HOLD. Unity's LoadSceneAsync loads the next scene and only then activates it:
  // its play-on-awake sounds (Baldi's greeting) start together with its first drawn frame. Here the
  // swap runs in one blocking frame (parse, build, texture preload), and the mixer thread kept
  // playing the new scene's voices through it -- the greeting under a frozen screen. The swap sets
  // the hold; the shell releases it once the new scene's first frame is presented (sceneShown()).
  // While held the mixer emits silence and its clock stands still, so held voices start whole.
  std::atomic<bool> audioHold{false};
  // AUDIO STATE LOCK. The mixer runs on the output thread (waveOut feed / RemoteIO callback) and
  // walks audioSources + the audioSim map, while game code on the main thread plays, stops and
  // destroys sources -- and Play/PlayOneShot INSERT into audioSim, which can rehash it under the
  // mixer's feet. The mixer holds this for one block; the main thread holds it for a frame's
  // game logic and for input dispatch (eng::AudioLock). Recursive: those scopes nest.
  std::recursive_mutex audioMutex;
  void (*loadPump)() = 0;   // shell hook, called during long loads (Windows: keep the window responsive)

  std::unordered_map<bb::NavMeshAgent*, AgentSim> agentSim;
  std::unordered_map<bb::CharacterController*, MoveSim> moveSim;
  int dbgFlags = 0; // eng::DebugFlags bits; written by eng::setDebugFlags from the shell's --debug
  // The loader mirrors every CharacterController with a stand-in CapsuleCollider so the object is
  // raycast/trigger-hittable, deliberately kept OUT of go->components (Unity's
  // GetComponent<CapsuleCollider>() returns null for a CharacterController). That also means the
  // destroy sweep -- which walks go->components -- can neither unregister nor free it, so the
  // shell has to be owned here and torn down with its CharacterController. Without this it stayed
  // in capsuleColliders as a dangling pointer after a scene unload.
  std::unordered_map<bb::CharacterController*, bb::CapsuleCollider*> ccShell;
  std::unordered_map<bb::Animator*, std::map<std::string, bool> > animBools;
  std::unordered_map<bb::Animator*, std::map<std::string, bool> > animTriggers;
  // TMP_InputField -> its TMP text component label (the pad answer box). Populated during
  // loader resolution; cleared with the scene registries so unloads leave no dangling labels.
  std::unordered_map<bb::InputField*, EngineUiText*> inputFieldLabel;
  std::unordered_map<bb::InputField*, EngineUiText*> inputFieldPlaceholder;
  bb::InputField* inputFocus = 0;   // ActivateInputField focus owner (TMP typed-input target)

  // Sprite-frame animation data (data/anims-index.json + data/anims-controllers.json, loaded once
  // by eng::loadAnims). Shared immutable data that survives scene swaps, like textures/fonts.
  struct SpriteAnimKeys {
    std::vector<std::pair<float, std::string> > keys; // (t, sprite guid), non-decreasing t
    float length = 0.0f;
    bool loop = false;
  };
  std::map<std::string, SpriteAnimKeys> spriteAnims;  // clip name -> keys
  std::map<std::string, std::string> controllerClips; // controller guid -> default-state clip
  // Full controller graph (tools/bake-controllers.js): states, their transitions, Any State
  // transitions and parameter types. Without it nothing could leave the default state.
  struct AnimCond { int mode = 0; std::string param; float threshold = 0.0f; }; // 1 If, 2 IfNot
  struct AnimTrans {
    std::string to;
    std::vector<AnimCond> conds;
    bool hasExitTime = false;
    float exitTime = 0.0f;
    bool canTransitionToSelf = true;
  };
  struct AnimStateDef { std::string clip; float speed = 1.0f; std::vector<AnimTrans> trans; };
  struct AnimCtrlDef {
    std::string defaultState;
    std::map<std::string, AnimStateDef> states;
    std::vector<AnimTrans> any;
    std::map<std::string, int> paramTypes;      // 1 float, 3 int, 4 bool, 9 trigger
    std::map<std::string, bool> boolDefaults;
  };
  std::map<std::string, AnimCtrlDef> animControllers;
  std::vector<NavPoly> navPolys;          // current scene's navmesh (empty: straight-line fallback)
  std::map<std::string, float> controllerSpeeds;      // controller guid -> default-state speed
  bool animsLoaded = false;

  // uGUI/TMP render pass state. tmpFonts is persistent (font-bake data, like other asset
  // registries); rect anchors live on the RectTransform components (die with the scene); the
  // draw list is rebuilt by every host render call.
  std::map<std::string, TMPFontData> tmpFonts;
  std::map<std::string, std::string> fontGuidToName;   // data/fonts-index.json cache
  bool fontsIndexLoaded = false;
  std::vector<DrawQuad> drawList;
  // Textures referenced by this build, in first-use order; DrawQuad::texId indexes it. Holds the
  // asset guid, which textures-index.json maps to data/tex/<guid>.raw (raw RGBA8, row 0 = top).
  std::vector<std::string> drawTextures;

  // Baked .asset meshes by guid, loaded on demand (eng-mesh.cpp). A null value is a cached MISS.
  // Not part of the scene registry: these are shared immutable assets and survive scene swaps,
  // exactly like the clip/texture registries. Freed by engineShutdown via freeMeshAssets().
  std::map<std::string, MeshAsset*> meshAssets;

  // Loader-created onClick listeners (SetActiveAction). Freed by engineShutdown; the scene keeps
  // them alive for its whole life because a Button's listener list outlives any single frame.
  std::vector<bb::UnityAction*> ownedActions;

  // Skybox materials by guid (eng-mesh.cpp), same sharing contract as meshAssets.
  std::map<std::string, SkyboxData*> skyboxAssets;

  std::vector<bb::Coroutine*> coroutines;

  // Trigger overlap state: the set of currently-overlapping collider pairs where at least one
  // side isTrigger. Canonically ordered (lower pointer first) so a pair has one identity.
  // Diffed each fixed step to synthesise Unity's Enter/Stay/Exit messages.
  std::set<std::pair<bb::Collider*, bb::Collider*> > triggerOverlaps;
  // Colliders destroyed while trigger messages are being dispatched. A handler is allowed to
  // Destroy things (pickups destroy themselves on contact), so the dispatcher checks this before
  // touching either side of a pair it snapshotted earlier in the same phase.
  std::set<bb::Collider*> triggerDead;

  // Components awaiting their first Start(). Unity runs Start NOT at creation but just before the
  // object's first Update, which is exactly the window a spawner uses to wire up the clone it just
  // made: NotebookScript does `Instantiate(learningGame)` then assigns `gc`/`baldiScript` on the
  // new MathGameScript -- whose own Start() immediately derefs `gc`. Running Start inside
  // Instantiate would hit a null (the prefab serializes `gc` as {fileID:0}).
  // Drained at the top of runUpdatePhase, before any Update call.
  std::vector<bb::Component*> pendingStarts;

  // PlayerPrefs (file-backed INI)
  bool prefsLoaded = false;
  std::string prefsPath = "run/prefs.ini";
  std::map<std::string, std::string> prefs;

  // deterministic RNG (fixed default seed -- parity runs must be reproducible)
  unsigned int rng[4];

  // input plumbing. keyEdge/keyHeld are indexed by Unity KeyCode value (types.h kKeySlots);
  // the injection side stamps BOTH edges (one-frame) and held state (until released).
  bool keyEdge[bb::kKeySlots];
  bool keyHeld[bb::kKeySlots];
  bool mouseEdge[8];
  bool mouseHeld[8];
  bool anyKeyFlag = false;
  std::map<std::string, bool> namedKeyEdge;
  std::map<std::string, float> axes;
  std::map<std::string, bool> namedButtons;

  State();
};

State& state();

// ---- registries ----------------------------------------------------------
std::vector<bb::GameObject*>& liveGameObjects(); // creation order (defines Find/FindObjectOfType order)
void noteGameObject(bb::GameObject* go);         // registry + lifetime tracking (scene/loader created)
void noteComponent(bb::Component* c);            // typed subsystem subscription (colliders/audio/nav)
void unnoteComponent(bb::Component* c);          // called by the destroy purge
void noteRuntimeObject(bb::Object* o);           // ownership for leak-free shutdown
void noteParent(bb::Transform* child, bb::Transform* parent); // hierarchy map (subtree destroy)
void noteTransform(bb::Transform* t);            // lifetime registry (ownership stays with the GameObject)
void queueDestroy(bb::GameObject* go, float delay); // Unity Destroy end-of-frame / delayed purge

// Concrete UiText used where a TMP label binds (loader creates one per TextMeshProUGUI comp).
// Dual base: Component (Unity's TextMeshProUGUI IS a Component, so this must be attachable to a
// GameObject for GetComponent<UiText>() to resolve -- DetentionTextScript::Start relies on it)
// + UiText (the TMP surface scripts call). Component supplies the Object base, so there is no
// ambiguous-base problem and `delete`ing through Component* runs the right destructor.
// OWNERSHIP: the GameObject owns this via go->components, exactly like any other component --
// deliberately NOT registered with noteRuntimeObject, which would double-free it at shutdown
// (engineShutdown deletes go->components AND ownedObjects).
struct EngineUiText : public bb::Component, public bb::UiText {
  std::string text;
  bool active = true;
  // TMP label metadata (m_* fields of the TextMeshProUGUI bake; defaults are Unity's)
  float fontSize = 32.0f;
  float colorR = 1.0f, colorG = 1.0f, colorB = 1.0f, colorA = 1.0f;
  int alignment = 257;                 // TextAlignmentOptions ints: 257 TopLeft .. 1026 BottomCenter
                                       // (256/512/1024 vertical bands OR'd with 1/2/4 horizontals)
  bool wordWrap = true;
  bool autoSize = false;
  // TMP m_characterSpacing / m_wordSpacing, in FONT UNITS like the glyph advances, so they scale
  // by the same pointScale. NotebookCount authors -1 and it is visible: ten gaps in "0 Notebooks"
  // x -1 x (32/24) = ~13 design px, ~40px at this canvas scale. Ignoring it made our HUD text
  // noticeably looser than the real game's. ItemText authors 0 -- which is exactly why THAT label
  // matched the reference pixel-for-pixel while this one did not.
  float characterSpacing = 0.0f;
  float wordSpacing = 0.0f;
  eng::TMPFontData* font = 0;          // m_fontAsset guid -> loaded font (0 = metrics fallback)
  void SetText(const std::string& s) override { text = s; }
  std::string GetText() const override { return text; }
  void SetActive(bool a) override { active = a; }
};

// ---- engine frame loop (canonical Unity order; sim/testbed only call runFullFrame) ----
void beginFrame(float unscaledDt);     // Time clocks + step counter
void runFixedSteps();                  // 50 Hz FixedUpdate (+ agent integration)
void runUpdatePhase();                 // Update + coroutine ticks (scaled dt)
void refreshAudioSpatial();            // cache listener + 3D source positions (main thread, each frame)
void loadAnims();                      // data/anims-index.json + anims-controllers.json into State
void runLateUpdatePhase();             // LateUpdate (screen shake et al)
void endFrame();                       // delayed destroys + audio clock + scene swaps
void runFullFrame(float unscaledDt);   // all four phases (headless sim uses this)
void drainPendingStarts();             // run queued Start()s now (runUpdatePhase + scene load)
// uGUI overlay capture: converts every ACTIVE Canvas-ancestor TMP label into DrawQuads. The
// host renders the returned list (SDL/GL backend, iOS GLKit, or the sim's assertions); the sim
// also uses it headlessly to verify label geometry, colors, and glyph metrics.
void clearDrawList();
void buildDrawList();
// Recompute collider overlaps and dispatch Unity's OnTriggerEnter/Stay/Exit. Runs once per FIXED
// step (Unity fires trigger messages from the physics step, not the Update phase).
void runTriggerPhase();

// ---- engine service hooks -------------------------------------------------
void setRandomSeed(unsigned int seed);     // deterministic Random for parity runs
void registerClipGuid(const std::string& guid, const std::string& relPath); // manifest -> audio path
bb::AudioClip* clipForGuid(const std::string& guid);  // binder lookup (creates the AudioClip node)
// data/manifest.json guid -> path under assetRoot (e.g. "AudioClip/.../BAL_Slap.wav"). Returns
// an EMPTY string when the guid is unknown. Implemented in eng-scene.cpp, where the manifest
// cache lives; clipForGuid needs it to give new clip nodes a real file to measure.
std::string assetRelForGuid(const std::string& guid);
bb::AudioClip* registerSceneClip(long long fid);      // scene-local clip node (no asset path)
float clipLength(bb::AudioClip* c);        // WAV header duration (cached; 1.0 fallback)
bool raycastImpl(const bb::Vec3& origin, const bb::Vec3& direction, bb::RaycastHit* hit,
                 float maxDistance, int layerMask, bb::QueryTriggerInteraction query);
// Seam to the translated scripts: wires a Button.onClick persistent call that targets a SCRIPT
// method (StartGame/ExitGame/UnpauseGame). Defined in game-actions.cpp, which can see the game
// types this layer cannot. Returns false when the method or target type is not handled.
bool wireScriptCall(bb::Button* button, bb::Component* target, const std::string& method,
                    int intArg = 0);
// Same seam for a serialized UnityEvent that lives on a GAME script's field (MouseoverScript
// mouseOver/mouseLeave, ...): the engine records (owner, field, target, method) generically; the
// game layer resolved to the actual UnityEvent member by field name. Returns false when the owner
// type / field / method is not handled.
bool wirePersistentEvent(bb::Component* owner, const std::string& field, bb::Component* target,
                         const std::string& method);
// Pointer-enter/exit dispatch to the scripts on a GameObject (Unity: EventSystem ->
// IPointerEnterHandler etc. on the components of the GO under the pointer). The engine hit-tests
// the Button; this seam lets the game layer respond (MouseoverScript underline swap).
bool dispatchPointerEvent(bb::GameObject* go, bool enter);
bb::Button* uiButtonAt(float px, float py);  // topmost interactable Button under the cursor
bool uiClick(float px, float py);            // hit test + onClick.Invoke; false = nothing there
// uGUI Slider dragging (pointer = touch id, or 0 for the mouse): press over an interactable slider
// captures it and sets the value from the pointer; moves keep setting it; release lets go.
bool uiSliderDown(int pointer, float px, float py);
void uiSliderMove(int pointer, float px, float py);
void uiSliderUp(int pointer);
void uiPointerMove(float px, float py);      // per-frame hover dispatch (enter/exit edges)
void engineShutdown();                     // frees every tracked runtime object (sim exit)
// Baked mesh lookup, cached by guid in State::meshAssets (misses cached too, as a null entry, so
// an unresolvable guid is reported once). Returns 0 when there is no usable blob -- a caller must
// SKIP that renderer, never substitute a primitive.
const MeshAsset* meshForGuid(const std::string& guid);
void freeMeshAssets();                     // drops the mesh cache (engineShutdown)
// Six-sided skybox lookup (data/skybox.json, cached like mesh blobs). Returns 0 when the guid is
// not a skybox material or the file has no usable data -- callers must keep the clear colour,
// not draw a substitute.
const SkyboxData* skyboxForMat(const std::string& matGuid);
// RenderSettings.m_SkyboxMaterial of the LIVE scene ('' = none/clear colour). Set by loadScene,
// cleared by unloadScene; eng-scene.cpp owns the storage.
const std::string& currentSceneSkybox();

// ---- platform shell plumbing ((SDL) testbed sets these; headless sim tests scripted values) ----
void setScreenSize(int w, int h);
void setPrefsPath(const std::string& p);   // file-backed PlayerPrefs target (run/prefs.ini)
void setAssetRoot(const std::string& root); // decompile Assets/ root used to read WAV headers

// ---- debug switches (the shell's --debug flag) --------------------------------------------------
// kDebugNoClip: CharacterController::Move skips blocker resolution entirely (fly-through walls).
// kDebugDiag:   collision diagnostics on stdout -- a collider census per loaded scene plus CCDBG
//               contact lines (collider name, push vector, penetration depth) on a frame cadence.
// The shell parses --debug (and the -debug alias) into kDebugNoClip | kDebugDiag so one flag both
// disables blocking and shows what WOULD have been hit at the raw target each step.
enum DebugFlags {
  kDebugNoClip = 1,
  kDebugDiag = 2,
  kDebugUi = 4,      // the -debug cheat menu (bottom-left hint + /-toggled panel)
};
void setDebugFlags(int flags);
int debugFlags();
void injectKey(int keyCodeIndex, bool pressedEdge);   // shell key event -> edge + held state
void injectAxis(const char* axisName, float value);   // held axis state per frame
void injectMouse(int button, bool pressedEdge);        // mouse button edge
// Release forms: clear held state with no edge, so a host driving InputManager's per-frame
// states (SDL testbed layer, or the sim) can hold a key across frames and end the hold cleanly.
void injectKeyRelease(int keyCodeIndex);
void injectMouseRelease(int button);
void injectAnyKey();                                    // any-key latches for one read

// ---- bake scene loader (eng-scene.cpp; bindings.cpp supplies factories + field binders) ----
void loadScene(const std::string& name);    // instantiate + bind data/scene-<Name>.bake.json
// TMP font registry: guid -> baked font (lazily parses data/font-*.json + the .atlas blob; the
// fonts-index.json cache is loaded on first miss). Persistent across scene swaps.
TMPFontData* fontForGuid(const std::string& guid);
// Instantiate data/prefab-<Name>.bake.json as a live subtree at pos/rot, running the same 4
// passes (objects+transforms, components, field binding, Awake/OnEnable/Start) the scene loader
// uses. Returns the clone's root GameObject, or 0 when no bake exists for that name (the caller
// then keeps the old inert-shell behaviour). Prefab bakes are parsed once and cached.
bb::GameObject* instantiatePrefab(const std::string& name, const bb::Vec3& pos, const bb::Quat& rot);
void unloadScene(bool keepDontDestroy);     // destroy non-DDOL tree (Awake-created DDOL survives)
std::string currentSceneName();             // live scene this frame
void processPendingScenes();                // consumes LoadScene requests at end-of-frame
void destroyGameObjectNow(bb::GameObject* go); // immediate teardown (scene unload sweep)

// Binding registries the generated binders / shell code read. The loader fills them.
struct BindCtx {
  std::unordered_map<long long, bb::GameObject*> goMap;  // GameObject fileID -> instance
  std::unordered_map<long long, bb::Component*> comps;   // component fileID -> instance
  std::unordered_map<long long, bb::Transform*> trs;     // Transform/RectTransform comps
  std::unordered_map<long long, bb::MeshRenderer*> mrs;  // DoorScript interior material swap
  std::unordered_map<long long, bb::UiText*> uiText;     // TMP comps -> UiText nodes
  // Slider -> its m_FillRect fileID. Deferred like any component reference: the RectTransform
  // may not be constructed yet when the Slider is read, so it is resolved after the walk.
  std::vector<std::pair<bb::Slider*, long long> > sliderFills;
  std::vector<std::pair<bb::Slider*, long long> > sliderHandles;   // m_HandleRect
  std::vector<std::pair<bb::RewiredTouchControl*, long long> > touchSticks; // TouchJoystick _stickTransform
  std::vector<std::pair<bb::Selectable*, long long> > selectableTargets; // m_TargetGraphic
  // Button m_OnClick persistent calls: Unity's inspector-wired listeners. 60 of the 63 Buttons in
  // these scenes carry them and they ARE the menu -- Start/Back/Options are all SetActive(bool)
  // pairs that show one screen and hide another. Deferred like every other reference: the target
  // may not be constructed when the Button is read.
  struct PendingCall {
    bb::Button* button;
    long long targetFileId;
    std::string method;
    bool boolArg;
    int intArg;       // m_Arguments.m_IntArgument (MathGame keypad: ButtonPress(0..9,-1,-2))
    std::string stringArg;  // m_Arguments.m_StringArgument (TMP_Text.set_text on the controls page)
  };
  std::vector<PendingCall> buttonCalls;
  std::vector<std::pair<bb::InputField*, long long> > inputFieldLabels;  // TMP_InputField m_TextComponent
  std::vector<std::pair<bb::InputField*, long long> > inputFieldPlaceholders;  // m_Placeholder label

  // SCRIPT-side serialized UnityEvents (MouseoverScript mouseOver/mouseLeave, UiSettings'
  // Toggle.onValueChanged-driven counterpart, ...): the call list lives on the GAME script's
  // serialized field, so unlike Button.m_OnClick the engine neither owns the event nor knows its
  // type. Recorded generically here (owner component + FIELD NAME + call) and resolved after the
  // component map is complete; wirePersistentEvent hands each resolved call to the game layer.
  struct PendingEventCall {
    bb::Component* owner;
    std::string fieldName;
    long long targetFileId;
    std::string method;
  };
  std::vector<PendingEventCall> eventCalls;

  // NOTE: the asset-node registries (prefabTemplates / materials / sprites / textures) are NOT
  // members -- they live in State. A BindCtx is a stack local of instantiateBake, so members would
  // die with each load, and the identity contract requires the opposite: one node per guid for the
  // whole run. Scripts compare these pointers (DoorScript swaps interior materials and tests
  // identity), and a DontDestroyOnLoad script carrying a Material* across a scene swap must still
  // match the node handed out afterwards for the same guid.

  // helpers shared with the generated binders (bindings.cpp); all four resolve against State
  bb::GameObject* prefabFor(const std::string& guid, const std::string& rel);
  bb::Material* materialFor(const std::string& guid);
  bb::Sprite* spriteFor(const std::string& guid);
  bb::Texture* textureFor(const std::string& guid);

  // ---- inline resolution rules (Unity serialization: {fileID:0} = null reference) -----------
  // fileIDs are read with asFileId(), never through `num`: the baker emits them as exact decimal
  // strings because Unity ids exceed 2^53 and a double would round distinct ids together.
  static const eng::JsonValue* refObj(const eng::JsonValue& f, const char* key) {
    const eng::JsonValue* v = f.find(key);
    if (!v || v->type != eng::JsonValue::Object) return 0;
    return eng::fileIdOf(*v, "fileID") != 0 ? v : 0;
  }
  // fileID of a ref object already validated by refObj().
  static long long refId(const eng::JsonValue* v) { return eng::fileIdOf(*v, "fileID"); }

  template <class T> void bindComp(const eng::JsonValue& f, const char* key, T*& out) {
    const eng::JsonValue* v = refObj(f, key);
    if (!v) return;
    std::unordered_map<long long, bb::Component*>::iterator it = comps.find(refId(v));
    if (it == comps.end()) return;
    out = dynamic_cast<T*>(it->second);
  }
  // GameObject-typed fields carry the GameObject's fileID when the target is scene-local.
  // Prefab-typed fields (GC bsodaSpray/alarmClock/quarter, NotebookScript learningGame) instead
  // serialize {fileID: <id inside the prefab file>, guid: <prefab asset>}: the fileID can never
  // hit goMap, so fall back to the inert prefab template shell keyed by guid.
  void bindGameObject(const eng::JsonValue& f, const char* key, bb::GameObject*& out) {
    const eng::JsonValue* v = refObj(f, key);
    if (!v) return;
    std::unordered_map<long long, bb::GameObject*>::iterator it = goMap.find(refId(v));
    if (it != goMap.end()) { out = it->second; return; }
    const eng::JsonValue* g = v->find("guid");
    if (g && g->type == eng::JsonValue::String && !g->str.empty()) {
      out = prefabFor(g->str, std::string());
    }
  }
  void bindTransformRef(const eng::JsonValue& f, const char* key, bb::Transform*& out) {
    const eng::JsonValue* v = refObj(f, key);
    if (!v) return;
    std::unordered_map<long long, bb::Transform*>::iterator it = trs.find(refId(v));
    if (it != trs.end()) out = it->second;
  }
  void bindMeshRendererRef(const eng::JsonValue& f, const char* key, bb::MeshRenderer*& out) {
    const eng::JsonValue* v = refObj(f, key);
    if (!v) return;
    std::unordered_map<long long, bb::MeshRenderer*>::iterator it = mrs.find(refId(v));
    if (it != mrs.end()) out = it->second;
  }
  void bindUiText(const eng::JsonValue& f, const char* key, bb::UiText*& out) {
    const eng::JsonValue* v = refObj(f, key);
    if (!v) return;
    std::unordered_map<long long, bb::UiText*>::iterator it = uiText.find(refId(v));
    if (it != uiText.end()) out = it->second;
  }
  void bindUiTextArray(const eng::JsonValue& f, const char* key, bb::UiText** out, int n) {
    const eng::JsonValue* v = f.find(key);
    if (!v || v->type != eng::JsonValue::Array) return;
    for (int i = 0; i < n && i < v->size(); i++) {
      const eng::JsonValue* el = v->at(i);
      if (!el || el->type != eng::JsonValue::Object) continue;
      std::unordered_map<long long, bb::UiText*>::iterator it = uiText.find(eng::fileIdOf(*el, "fileID"));
      if (it != uiText.end()) out[i] = it->second;
    }
  }
  // prefab-typed asset guid -> template GameObject shell
  void bindPrefab(const eng::JsonValue& f, const char* key, bb::GameObject*& out) {
    const eng::JsonValue* v = f.find(key);
    if (!v || v->type != eng::JsonValue::Object) return;
    const eng::JsonValue* g = v->find("guid");
    if (!g || g->type != eng::JsonValue::String || g->str.empty()) return;
    out = prefabFor(g->str, std::string());
  }
  void bindAudioClip(const eng::JsonValue& f, const char* key, bb::AudioClip*& out) {
    const eng::JsonValue* v = f.find(key);
    if (!v || v->type != eng::JsonValue::Object) return;
    const eng::JsonValue* g = v->find("guid");
    if (!g || g->type != eng::JsonValue::String || g->str.empty()) return;
    out = clipForGuid(g->str);
  }
  // asset-node refs by guid (Material/Sprite/Texture keep distinct identities for pointer tests)
  void bindMaterial(const eng::JsonValue& f, const char* key, bb::Material*& out) {
    const eng::JsonValue* v = f.find(key);
    if (!v || v->type != eng::JsonValue::Object) return;
    const eng::JsonValue* g = v->find("guid");
    if (!g || g->type != eng::JsonValue::String || g->str.empty()) return;
    out = materialFor(g->str);
  }
  void bindSprite(const eng::JsonValue& f, const char* key, bb::Sprite*& out) {
    const eng::JsonValue* v = f.find(key);
    if (!v || v->type != eng::JsonValue::Object) return;
    const eng::JsonValue* g = v->find("guid");
    if (!g || g->type != eng::JsonValue::String || g->str.empty()) return;
    out = spriteFor(g->str);
  }
  void bindTexture(const eng::JsonValue& f, const char* key, bb::Texture*& out) {
    const eng::JsonValue* v = f.find(key);
    if (!v || v->type != eng::JsonValue::Object) return;
    const eng::JsonValue* g = v->find("guid");
    if (!g || g->type != eng::JsonValue::String || g->str.empty()) return;
    out = textureFor(g->str);
  }

  // ---- scalar value binding (absent key -> keep the C++ default, i.e. the C# initializer) ----
  void bindFloat(const eng::JsonValue& f, const char* key, float& out) {
    const eng::JsonValue* v = f.find(key);
    if (v && v->type == eng::JsonValue::Number) out = v->asFloat(out);
  }
  void bindInt(const eng::JsonValue& f, const char* key, int& out) {
    const eng::JsonValue* v = f.find(key);
    if (v && v->type == eng::JsonValue::Number) out = v->asInt(out);
  }
  void bindBool(const eng::JsonValue& f, const char* key, bool& out) {
    const eng::JsonValue* v = f.find(key);
    if (v && v->type != eng::JsonValue::Null) out = v->asBool(out);
  }
  void bindString(const eng::JsonValue& f, const char* key, std::string& out) {
    const eng::JsonValue* v = f.find(key);
    if (!v) return;                      // absent private field: C++ default stays
    if (v->type == eng::JsonValue::String) out = v->str;
    else if (v->type == eng::JsonValue::Null) out = std::string(); // null string -> ""
  }
  void bindVec3(const eng::JsonValue& f, const char* key, bb::Vec3& out) {
    const eng::JsonValue* v = f.find(key);
    if (v && v->type == eng::JsonValue::Object) {
      const eng::JsonValue* x = v->find("x"); const eng::JsonValue* y = v->find("y");
      const eng::JsonValue* z = v->find("z");
      out = bb::Vec3(x ? x->asFloat() : 0.0f, y ? y->asFloat() : 0.0f, z ? z->asFloat() : 0.0f);
    }
  }
  void bindVec2(const eng::JsonValue& f, const char* key, bb::Vec2& out) {
    const eng::JsonValue* v = f.find(key);
    if (v && v->type == eng::JsonValue::Object) {
      const eng::JsonValue* x = v->find("x");
      const eng::JsonValue* y = v->find("y");
      out = bb::Vec2(x ? x->asFloat() : 0.0f, y ? y->asFloat() : 0.0f);
    }
  }
  void bindColor(const eng::JsonValue& f, const char* key, bb::Color& out) {
    const eng::JsonValue* v = f.find(key);
    if (v && v->type == eng::JsonValue::Object) {
      const eng::JsonValue* r = v->find("r"); const eng::JsonValue* g = v->find("g");
      const eng::JsonValue* b = v->find("b"); const eng::JsonValue* a = v->find("a");
      out = bb::Color(r ? r->asFloat() : 1.0f, g ? g->asFloat() : 1.0f,
                      b ? b->asFloat() : 1.0f, a ? a->asFloat() : 1.0f);
    }
  }

  // ---- array channels (bake arrays keep exact slots; null slots stay default) ------------------
  template <class T> void bindCompArray(const eng::JsonValue& f, const char* key, T** out,
                                        int n) {
    const eng::JsonValue* v = f.find(key);
    if (!v || v->type != eng::JsonValue::Array) return;
    for (int i = 0; i < n && i < v->size(); i++) {
      const eng::JsonValue* el = v->at(i);
      if (!el || el->type != eng::JsonValue::Object) continue;
      const long long fid = eng::fileIdOf(*el, "fileID");
      if (fid == 0) continue;
      std::unordered_map<long long, bb::Component*>::iterator it = comps.find(fid);
      if (it != comps.end()) out[i] = dynamic_cast<T*>(it->second);
    }
  }
  void bindGOArray(const eng::JsonValue& f, const char* key, bb::GameObject** out, int n) {
    const eng::JsonValue* v = f.find(key);
    if (!v || v->type != eng::JsonValue::Array) return;
    for (int i = 0; i < n && i < v->size(); i++) {
      const eng::JsonValue* el = v->at(i);
      if (!el || el->type != eng::JsonValue::Object) continue;
      const long long fid = eng::fileIdOf(*el, "fileID");
      if (fid == 0) continue;
      std::unordered_map<long long, bb::GameObject*>::iterator it = goMap.find(fid);
      if (it != goMap.end()) { out[i] = it->second; continue; }
      const eng::JsonValue* g = el->find("guid");   // prefab element: template shell by guid
      if (g && g->type == eng::JsonValue::String && !g->str.empty()) {
        out[i] = prefabFor(g->str, std::string());
      }
    }
  }
  void bindTrArray(const eng::JsonValue& f, const char* key, bb::Transform** out, int n) {
    const eng::JsonValue* v = f.find(key);
    if (!v || v->type != eng::JsonValue::Array) return;
    for (int i = 0; i < n && i < v->size(); i++) {
      const eng::JsonValue* el = v->at(i);
      if (!el || el->type != eng::JsonValue::Object) continue;
      const long long fid = eng::fileIdOf(*el, "fileID");
      if (fid == 0) continue;
      std::unordered_map<long long, bb::Transform*>::iterator it = trs.find(fid);
      if (it != trs.end()) out[i] = it->second;
    }
  }
  void bindClipArray(const eng::JsonValue& f, const char* key, bb::AudioClip** out, int n) {
    const eng::JsonValue* v = f.find(key);
    if (!v || v->type != eng::JsonValue::Array) return;
    for (int i = 0; i < n && i < v->size(); i++) {
      const eng::JsonValue* el = v->at(i);
      if (!el || el->type != eng::JsonValue::Object) continue;
      const eng::JsonValue* g = el->find("guid");
      if (g && g->type == eng::JsonValue::String && !g->str.empty()) {
        out[i] = clipForGuid(g->str);           // asset clip: manifest path -> WAV duration
        continue;
      }
      const long long fid = eng::fileIdOf(*el, "fileID"); // scene-local clip node fallback
      if (fid != 0) out[i] = registerSceneClip(fid);
    }
  }
  void bindSprArray(const eng::JsonValue& f, const char* key, bb::Sprite** out, int n) {
    const eng::JsonValue* v = f.find(key);
    if (!v || v->type != eng::JsonValue::Array) return;
    for (int i = 0; i < n && i < v->size(); i++) {
      const eng::JsonValue* el = v->at(i);
      if (!el || el->type != eng::JsonValue::Object) continue;
      const eng::JsonValue* g = el->find("guid");
      if (g && g->type == eng::JsonValue::String && !g->str.empty()) out[i] = spriteFor(g->str);
    }
  }
  void bindTexArray(const eng::JsonValue& f, const char* key, bb::Texture** out, int n) {
    const eng::JsonValue* v = f.find(key);
    if (!v || v->type != eng::JsonValue::Array) return;
    for (int i = 0; i < n && i < v->size(); i++) {
      const eng::JsonValue* el = v->at(i);
      if (!el || el->type != eng::JsonValue::Object) continue;
      const eng::JsonValue* g = el->find("guid");
      if (g && g->type == eng::JsonValue::String && !g->str.empty()) out[i] = textureFor(g->str);
    }
  }
  void bindIntArray(const eng::JsonValue& f, const char* key, int* out, int n) {
    const eng::JsonValue* v = f.find(key);
    if (!v) return;
    // Unity serialises a primitive int[] as ONE packed hex string -- 8 hex digits per element,
    // little-endian int32 (GameControllerScript.itemSelectOffset: "b0ffffffd8ffffff00000000" =
    // {-80, -40, 0}). Only JSON arrays were accepted, so the offsets stayed {0,0,0} and the red
    // item selector never moved off its first slot. Decoded here, where the field is KNOWN to be
    // an int array, so a hex-looking string elsewhere (a guid) can never be misread.
    if (v->type == eng::JsonValue::String) {
      const std::string& hx = v->str;
      for (int i = 0; i < n && (size_t)(i * 8 + 8) <= hx.size(); i++) {
        unsigned int u = 0;
        for (int b = 0; b < 4; b++) {                       // byte b of the little-endian word
          const unsigned int byte = (unsigned int)strtoul(hx.substr((size_t)(i * 8 + b * 2), 2).c_str(), 0, 16);
          u |= byte << (8 * b);
        }
        out[i] = (int)u;
      }
      return;
    }
    if (v->type != eng::JsonValue::Array) return;
    for (int i = 0; i < n && i < v->size(); i++) {
      const eng::JsonValue* el = v->at(i);
      if (el && el->type == eng::JsonValue::Number) out[i] = el->asInt();
    }
  }
};

// bindings.cpp contract (generated from src/game/*.hpp by tools/bind-codegen.js):
bb::Component* makeScript(const std::string& className); // null = no factory (loader logs once)
void bindScript(const std::string& className, bb::Component* c, const eng::JsonValue& f,
                eng::BindCtx& ctx);

} // namespace eng
