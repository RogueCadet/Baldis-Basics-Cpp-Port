// eng.h -- engine-facing API surface used by translated scripts.
// Phase 2 (engine-lite) provides the definitions; declarations mirror Unity semantics 1:1
// so the C# -> C++ translations keep original field/method names and serialized bindings.
// NOTE: intentionally declaration-only until the engine-lite implementation lands.
#pragma once
#include <string>
#include "bbmath.h"
#include "types.h"

namespace bb {

struct Object {
  virtual ~Object() {}
  std::string name;
};

struct GameObject;
struct Collider; // fwd (trigger hooks below + raycast hits)
void Destroy(GameObject* obj);
void Destroy(GameObject* obj, float delay); // Unity delayed form (0f = at end of frame; AlarmClock
                                            // self-destruct); engine impl Phase 2
void DontDestroyOnLoad(Object* obj);
bool isPlaying();
// implemented by the scene manager; instantiated when a GameObject of that component exists
template <class T> T* FindObjectOfType();
template <class T> T* GetComponent_impl(GameObject* go); // per-type engine impls (Phase 2)

struct PlayerPrefs {
  static std::string GetString(const std::string& k, const std::string& def = "");
  static int GetInt(const std::string& k, int def = 0);
  static float GetFloat(const std::string& k, float def = 0.0f); // used by player prefs (sensitivity)
  static void SetString(const std::string& k, const std::string& v);
  static void SetInt(const std::string& k, int v);
  static void SetFloat(const std::string& k, float v);
  static void Save(); // engine impl (Unity's explicit flush; setters already persist each time)
  static bool HasKey(const std::string& k); // engine impl (OptionsManager first-run gate)
};

// TMP_Text replacement: engine, TTF-rasterized text drawn as textured quads.
struct UiText {
  virtual ~UiText() {}
  virtual void SetText(const std::string& s) = 0;
  virtual std::string GetText() const = 0; // TMP .text read (EndlessTextScript appends to itself)
  virtual void SetActive(bool a) = 0;
  int fontStyle = 0;                   // TMP FontStyles mirror (TextUnderliner; values in types.h)
};

// TMP FontStyles (subset used by script code; full TMP table when other styles show up)
enum class FontStyles : int {
  Normal = 0,
  Bold = 1,
  Italic = 2,
  Underline = 4,
};

// Unity TouchScreenKeyboardType (NoKeyboardScript casts (-1) to hide the on-screen keyboard on
// touch UI text fields; any int value is legal through the explicit cast)
enum class TouchScreenKeyboardType : int {
  Default = 0,
};

struct Transform {
  virtual ~Transform() {}              // polymorphic: RectTransform derives from Transform (UI);
                                       // hierarchy sweeps may also delete through the registry
  // Unity sibling index (the bake's `rootOrder`). A Canvas paints in HIERARCHY order -- depth
  // first, by sibling index -- so the uGUI draw list is sorted on this, not on creation order.
  // Without it MainMenu's Background (rootOrder 0) painted LAST and covered the whole menu.
  int siblingIndex = 0;
  Vec3 position;
  Vec3 localPosition;                  // Unity Transform.localPosition (hierarchy sync Phase 2;
                                       // PickupAnimationScript bobs it each frame)
  Quat rotation;
  Vec3 scale = Vec3::one();
  Transform* parent = 0;
  GameObject* go = 0;                  // owning object (scene loader binds; Transform.gameObject/tag)
  // Hierarchy pass bookkeeping (eng::propagateHierarchy): the pose as of the end of the previous
  // pass, so a parent's MOVE can be applied to its children and a script's localPosition write
  // can be told apart from a world write. Engine-only; scripts never touch these.
  Vec3 hierPos, hierLocal, hierScale;
  Quat hierRot;
  bool hierValid = false;
  bool hierMoved = false;
  Vec3 forward() const { return rotation.rotate(Vec3(0.0f, 0.0f, 1.0f)); } // Unity transform.forward
  Vec3 right() const { return rotation.rotate(Vec3(1.0f, 0.0f, 0.0f)); }   // Unity transform.right
  std::string name() const; // engine impl: forwards to owning object's name (Unity: Transform.name)
  std::string tag() const;  // engine impl: forwards to owning object's tag (Unity: Component.tag via Transform)
  GameObject* gameObject() const; // engine impl (owner object; Unity Transform.gameObject)
  Vec3 eulerAngles() const { return rotation.eulerAngles(); } // Unity Transform.eulerAngles (delegates)
  void Rotate(const Vec3& euler, bool worldSpace = false); // engine impl (Unity Rotate; default Space.Self:
                                                           // rotation = rotation * euler. FirstPrizeScript
                                                           // call sites are all 1-arg self-space)
  void LookAt(const Vec3& target, const Vec3& worldUp = Vec3(0.0f, 1.0f, 0.0f)); // engine impl; 1-arg form = Unity's default worldUp
};

struct Component : public Object {  // full def with auto transform binding arrives in Phase 2
  // NB: the inherited Object::name stays EMPTY on components. Unity's Component.name is the owning
  // GameObject's name, so a C# `collider.name` translates to `collider->gameObject->name` (reading
  // the field silently broke the quarter machines/payphone, tape player, scissors, Office Trigger).
  GameObject* gameObject = 0;
  Transform* transform = 0;
  bool enabled = true;
  bool isActiveAndEnabled() const;    // engine impl
  std::string tag();                  // engine impl: forwards to gameObject->tag (Unity Component.tag)
  bool CompareTag(const char* t) { return tag() == t; } // Unity CompareTag; Phase 3 swaps to an
                                                        // allocation-free compare when tag() gains
                                                        // an interned-table impl
  template <class T> T* GetComponent() { return GetComponent_impl<T>(gameObject); } // body finds the per-type engine impl in Phase 2

  virtual void Awake() {}
  virtual void OnEnable() {}           // Unity OnEnable (fires when the object turns active/enabled)
  virtual void OnDisable() {}         // Unity: the component stops being active in the hierarchy
  // Unity never calls FixedUpdate/Update/LateUpdate before Start. Set while the component sits in
  // State::pendingStarts (script components, queued at load/Instantiate), cleared when its Start
  // runs; the update loops skip it meanwhile. Engine-built components are never queued.
  bool startPending = false;
  virtual void Start() {}
  // The BASE bodies run only for a class that does not override them, so they mark the component
  // and the phase loops skip it afterwards (noOp bits: 1 Update, 2 FixedUpdate, 4 LateUpdate).
  // ~10k components per School frame, almost all without these, each cost a virtual call per
  // phase per frame (and per physics step) -- a real share of an iOS-6-class CPU's frame.
  unsigned char noOp = 0;
  virtual void Update(float dt) { noOp |= 1; }
  virtual void FixedUpdate() { noOp |= 2; }      // Unity fixed-timestep hook (engine scheduler, Phase 2)
  virtual void LateUpdate() { noOp |= 4; }
  virtual void OnDestroy() {}
  // Unity passes the Collider; trigger hooks fire from the Phase 3/b engine layer
  virtual void OnTriggerEnter(Collider* other) {}
  virtual void OnTriggerStay(Collider* other) {}
  virtual void OnTriggerExit(Collider* other) {}
  // Unity OnApplicationPause: sent when the app is suspended (true) / resumed (false). Only the iOS
  // shell sends it (eng::applicationPause); the Windows testbed mirrors the PC build, which never
  // reacts to focus loss.
  virtual void OnApplicationPause(bool pauseStatus) {}
};

// ---- raycast surface (Phase 3 implements hit queries; used by UseItem / interactions) ----
struct Ray {
  Vec3 origin, direction;
};

struct Collider;
struct RaycastHit { // Unity field subset used by scripts
  Collider* collider = 0;
  GameObject* gameObject = 0;
  Transform* transform = 0;
  Vec3 point;
  float distance = 0.0f;
};

struct Physics {
  static bool Raycast(const Ray& ray, RaycastHit* hit); // engine impl (AABB + triggers, Phase 3)
  // Unity's origin+direction overload with distance mask + trigger policy (Baldi line-of-sight)
  static bool Raycast(const Vec3& origin, const Vec3& direction, RaycastHit* hit,
                      float maxDistance, int layerMask, QueryTriggerInteraction query);
  static bool Raycast(const Vec3& origin, const Vec3& direction, RaycastHit* hit,
                      float maxDistance, int layerMask); // same, QueryTriggerInteraction.UseGlobal
};

// ---- uGUI settings controls (options screen + menu navigation) ----
// (placed after the Physics/Component core so the base class is complete; moved on first smoke)

// The input-routing singleton for focus navigation (UIController). Declared before Selectable
// because Selectable::OnSelect takes one.
struct EventSystem : public Component {
  static EventSystem* current();       // engine impl
  GameObject* currentSelectedGameObject() const; // engine impl (0 when nothing selected)
};

// uGUI Selectable: the shared base of every interactive uGUI control.
//
// This MUST mirror Unity's hierarchy (Selectable -> Button/Toggle/Slider/InputField/Scrollbar),
// not sit beside it. Script fields are typed by Unity's base class -- UIController declares
// `Selectable* firstButton/dummyButtonPC` and the scene points them at Button components -- and
// the binder resolves such fields with dynamic_cast across Component*. With a flat hierarchy that
// cast fails, the field binds null, and UIController::OnEnable derefs it on the first load.
// Keep Selectable declared ahead of its subclasses; eng.h is include-order sensitive.
struct Image;
struct Sprite;
struct Selectable : public Component {
  void Select();                       // engine impl (uGUI selection)
  void OnSelect(EventSystem* e);       // engine impl (e == 0: null baseEventData)
  // Unity Selectable TRANSITION (serialized m_Transition / m_Colors / m_SpriteState /
  // m_TargetGraphic). MainMenu alone has 18 Sprite Swap buttons whose highlighted sprite is the
  // underlined "*_Selected" art and 38 Color Tint buttons; without this nothing reacted to hover
  // except the two MouseoverScript labels. Colours are float[4] because Color is declared below.
  int transition = 1;                  // 0 None, 1 ColorTint (Unity default), 2 SpriteSwap, 3 Animation
  float normalColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
  float highlightedColor[4] = { 0.9607843f, 0.9607843f, 0.9607843f, 1.0f };
  float colorMultiplier = 1.0f;
  Sprite* highlightedSprite = 0;       // m_SpriteState.m_HighlightedSprite
  Image* targetImage = 0;              // m_TargetGraphic, when it is an Image (all of this game's)
  bool interactable = true;            // Unity Selectable.interactable (m_Interactable): no click/hover when false
  std::string normalTrigger = "Normal";           // m_AnimationTriggers (Animation transition)
  std::string highlightedTrigger = "Highlighted";
};

// Unity UnityAction delegate + UnityEvent (OnAwakeTrigger et al): scene-serialized persistent call
// lists PLUS runtime AddListener from Button scripts. Member-method listeners get a tiny adapter
// struct per script (C# `new UnityAction(OpenScreen)` binding). Declared before Selectable's
// subclasses (Button::onClick and Toggle::onValueChanged both own a UnityEvent).
struct UnityAction {
  virtual void Invoke() = 0;
  virtual ~UnityAction() {}
};
struct UnityEvent {
  void Invoke();                       // executes runtime listeners (+ loader-bound persits later)
  void AddListener(UnityAction* a);    // engine impl (runtime listeners; engine owns list life)
  std::vector<UnityAction*> listeners; // runtime listener list (adapter structs live w/ owner scripts)
};

struct Toggle : public Selectable {
  bool isOn = false;                   // Unity Toggle.isOn
  UnityEvent onValueChanged;           // uGUI serialized calls (UiSettings radio groups)
};

struct Camera : public Component {   // Unity: Behaviour -> Component (has gameObject/transform/GetComponent)
  int cullingMask = -1;              // LayerMask bits; saved at Start / restored after learning games
  float fov = 60.0f, nearClip = 0.3f, farClip = 1000.0f;
  Ray ScreenPointToRay(const Vec3& screenPos) const; // engine impl (Phase 2)
  static Camera* main();
};

// Unity Material. Identity-only to scripts (DoorScript compares pointers), plus the main texture
// guid so the render pass can bind the baked blob -- see data/material-textures.json.
struct Material : public Object {
  std::string mainTexGuid;           // empty = flat-colour material (BlackCeiling, BSODA_Side)
  // Unity _MainTex tiling/offset (material m_TexEnvs m_Scale / m_Offset). A surface that
  // samples the image ONCE when the material says (2,2) draws its detail at double size --
  // which is exactly how the walls differed from a real screenshot. 7 of the 74 textured
  // materials tile; the rest are (1,1) and unaffected.
  Vec2 texScale = Vec2(1.0f, 1.0f);
  Vec2 texOffset = Vec2(0.0f, 0.0f);
  // Unity _Color: the material TINT, multiplied into the sampled texel. These materials are NOT
  // white -- 53 of the 74 textured ones are 0.96 grey and 7 are 0.48 (the Dark_ variants).
  // Drawing them white made every surface a uniform 4.2% too bright against a real screenshot,
  // which is exactly 1/0.96, and the dark materials twice as bright as they should be.
  float colorR = 1.0f, colorG = 1.0f, colorB = 1.0f, colorA = 1.0f;
  // Unity m_ShaderKeywords contains _ALPHATEST_ON: Standard's cutout variant. The world pass
  // must alpha-test these draws (cutoff 0.5) or the transparent texels paint SOLID -- the
  // window pane quads occlude the room behind their glass. 11 game materials set this
  // (Window, Dark_Window, the door materials, fence); every other material is false.
  bool alphaTest = false;
};
// Unity MeshRenderer. Derives Object, NOT Component: Unity's Renderer is not a MonoBehaviour and
// scripts only hold it to swap .material (EntranceScript finale). The world render pass needs
// more than that, so it also carries its owner and its geometry -- the level is built entirely
// from two Unity built-in primitives (same as the MeshCollider work), so the mesh is a primitive
// id rather than vertex data.
struct MeshRenderer : public Object {
  Material* material = 0;            // swap target used by EntranceScript finale
  GameObject* gameObject = 0;        // owner (Unity Renderer.gameObject)
  Transform* transform = 0;          // owner transform -- supplies the quad's world matrix
  bool enabled = true;               // Unity Renderer.enabled
  // Mesh fileID of the sibling MeshFilter, stored RAW. 10210 = built-in Quad (1x1, XY plane,
  // normal +Z) and 10209 = built-in Plane (10x10, XZ) are the synthesised primitives; 0 means no
  // MeshFilter. 4300000 is a REAL .asset mesh (the CombinedMeshes_* desks/lockers/BSODA sides)
  // with baked vertex data in data/mesh/<meshGuid>.mesh -- never substitute a primitive for one,
  // which drew giant grey planes through the room.
  int meshPrimitive = 0;
  // GUID of the referenced mesh asset. REQUIRED to tell the .asset meshes apart: the fileID is
  // 4300000 for every one of them, so the id alone identifies the KIND of reference and the guid
  // identifies the mesh. Empty for the built-in primitives (their guid is the built-in library).
  std::string meshGuid;
};

// Unity Renderer base (Unity chain: Object -> Component -> Behaviour -> Renderer). Our
// MeshRenderer predates this hook and stays Object-derived for now -- Phase 2/3 unifies the
// hierarchy if any script needs more than the pointer (BullyScript field use).
// SpriteRenderer DOES sit under it exactly like Unity: CraftersScript's serialized
// `craftersRenderer` (Renderer*) points at Arts' own SpriteRenderer in School.unity (both fields
// carry the same fileID 20876), and that pointer was left null by the binder -- the crash
// CraftersScript::FixedUpdate hit the moment notebooks reached 7 (visible in
// %LOCALAPPDATA%\CrashDumps\baldi.exe.11808.dmp: AV at bb::Renderer::isVisible+0x18, rcx=0).
struct Renderer : public Component {
  bool isVisible() const;            // engine impl (CraftersScript checks camera visibility)
};

struct Color {
  float r = 1, g = 1, b = 1, a = 1;
  Color() {}
  Color(float r_, float g_, float b_, float a_) : r(r_), g(g_), b(b_), a(a_) {}
  static Color red() { return Color(1.0f, 0.0f, 0.0f, 1.0f); } // Unity Color.red
};

struct Sprite : public Object {};    // engine: texture-backed quad (atlas region)

// Unity SpriteRenderer. This is what every BILLBOARD in the game is: Baldi, the NPCs, the
// pickups and the wall clutter are all a camera-facing textured quad, NOT mesh geometry (the
// Billboard component spins the transform in LateUpdate; the renderer just draws the quad).
// The quad's world size is NOT stored here -- Unity derives it as pixels / spritePixelsToUnits
// with spritePivot as the origin, which the bake writes to data/sprites-index.json.
struct SpriteRenderer : public Renderer {
  Sprite* sprite = 0;            // sprite->name IS the texture guid (BindCtx::spriteFor)
  Color color;                   // m_Color, multiplied into the quad (tint + fade)
  int sortingOrder = 0;          // Unity sorting within the layer; ties broken by depth
};

struct Image : public Component {    // uGUI Image
  Sprite* sprite = 0;
  Color color;
  Sprite* overrideSprite = 0;          // Unity Image.overrideSprite: drawn instead of `sprite` (Sprite Swap)
  Color tint;                          // CanvasRenderer colour: multiplies `color` (Color Tint)
  bool raycastTarget = true;           // Graphic.m_RaycastTarget: blocks touches to lower canvases
};

struct Texture : public Object {};   // engine: GL texture (Phase 2; itemTextures + RawImage)

struct RawImage : public Component { // uGUI RawImage (item slots display itemTextures)
  Texture* texture = 0;
  Color color;                         // Unity RawImage.color (tint; white when unauthored)
  bool raycastTarget = true;           // Graphic.m_RaycastTarget
};

struct RectTransform;   // defined with the UI types below; Slider only holds a pointer
struct Slider : public Selectable {  // uGUI Slider (staminaBar); Unity: Slider -> Selectable
  float value = 0.0f;
  float minValue = 0.0f;
  float maxValue = 1.0f;
  // Unity's Slider DRIVES this rect: the fill's anchors are recomputed from the normalised value
  // every frame, which is why the bake stores it with sizeDelta (0,0) and collapsed anchors. Not
  // following that left the stamina bar as its empty red track with no green fill at all.
  RectTransform* fillRect = 0;
  // ...and the handle (m_HandleRect): anchorMin.x = anchorMax.x = normalised value. Its PARENT
  // (the Handle Slide Area) is the track a pointer drags along (uGUI Slider.UpdateDrag).
  RectTransform* handleRect = 0;
  int direction = 0;                 // m_Direction: 0 LeftToRight, 1 RightToLeft, 2 BottomToTop, 3 TopToBottom
  bool wholeNumbers = false;         // m_WholeNumbers
  bool interactable = true;          // m_Interactable (the stamina bar's is display-only)
  float normalized() const {
    const float span = maxValue - minValue;
    if (span <= 0.0f) return 0.0f;
    const float t = (value - minValue) / span;
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  }
};

struct Button : public Selectable {  // uGUI Button; Unity: Button -> Selectable
  UnityEvent onClick;                  // listeners added via AddListener (BackButton/BasicButton)
};

// uGUI Input Field -- TMP_InputField in the C#; rendered by the engine raster font (Phase 2).
// Unity: TMP_InputField -> Selectable (NoKeyboardScript derives this type).
struct InputField : public Selectable {
  std::string text;
  int keyboardType = 0;                // Unity InputField.keyboardType (TouchScreenKeyboardType)
  // TMP_InputField.text setter parity: TMP InputFields immediately push the value into their
  // m_TextComponent label (the pad answer box shows typed digits). Scripts use SetText; the
  // direct `text` field stays for reads + bind-time input. The label pointer resolves at load.
  void SetText(const std::string& value);
  void ActivateInputField();         // engine impl (focus + caret)
};

// Hand-coded animation parameters + the sprite-frame driver. Baldi and the NPCs are ANIMATOR-
// driven (BaldiTutor's controller plays BaldiWave; BaldiFeed talks during the math game), not
// script sprite arrays. The loader binds controller.guid (the scene bake field); Update() samples
// the clip's sprite keys from data/anims-index.json and swaps the gameobject's SpriteRenderer,
// which replaces Unity's Animator -> SpriteRenderer binding for this game's PPtr-only clips.
struct Animator : public Component {
  std::string controllerGuid;  // bake: Animator.controller.guid (data/anims-controllers.json key)
  std::string clipName;        // resolved clip ("" = nothing bound; Update is inert)
  float clipSpeed = 1.0f;      // AnimatorState.m_Speed -- BaldiWave carries 0.5
  float clipTime = 0.0f;       // sampled in clip seconds (Unity normalisedTime * length)
  std::string stateName;       // current AnimatorState (controller graph); "" = not bound yet
  int updateMode = 0;          // Unity AnimatorUpdateMode: 0 Normal, 1 AnimatePhysics, 2 UnscaledTime
  bool clipPlaying = false;    // false once a non-looping clip has run past its end
  void Play(const char* stateName);          // switch state (data/anims-controllers.json states)
  void SetBool(const char* name, bool value);
  bool GetBool(const char* name) const; // engine impl (flag table)
  void SetTrigger(const char* name);
  void Update(float dt) override;      // sprite-frame sampling; no-op without a bound controller
};
struct Canvas : public Component {   // uGUI Canvas root (hud enabled toggle)
  int sortingOrder = 0;              // Unity Canvas.sortingOrder: overlay canvases paint in this order
};

// Unity: RectTransform DERIVES from Transform (a GameObject carries exactly one transform, and
// a UI object's transform IS its rect). Modeled as a dual base: bb::Transform keeps the TRS +
// parent chain so the whole transform registry/hierarchy machinery applies to rect nodes too,
// and Component supplies the identity plumbing (gameObject, enabled, component lookups).
// position/localPosition are therefore the BASE fields (the loader composes them); the
// RectTransform-only members below are the anchored fields the loader pairs from the bake.
struct RectTransform : public Transform, public Component {
  // Unity name parity under the dual base: unqualified `rect.gameObject`, `.transform`, `.tag`,
  // `.CompareTag` resolve to the Component members (Unity RectTransform is "just" a Transform
  // with these same members). Without these the dual base makes them ambiguous at script sites.
  using bb::Component::gameObject;
  using bb::Component::transform;
  using bb::Component::tag;
  using bb::Component::CompareTag;
  Vec2 anchoredPosition;               // Unity RectTransform.anchoredPosition
  Vec2 anchorMin{ 0.5f, 0.5f };        // anchor corner in parent-rect space (0..1)
  Vec2 anchorMax{ 0.5f, 0.5f };        // anchor corner in parent-rect space (0..1)
  Vec2 sizeDelta{ 100.0f, 100.0f };    // added to the anchor span to form the pixel rect
  Vec2 pivot{ 0.5f, 0.5f };            // pivot (relative to the rect, 0..1)
  // LOCAL scale straight from the bake. The composed world `scale` is useless for UI: Unity
  // serialises every Screen Space Overlay canvas ROOT at scale (0,0,0) (the Canvas drives it at
  // runtime), so every descendant's composed scale is 0. layChainScaled reads this instead.
  Vec2 localScale{ 1.0f, 1.0f };
  // VerticalLayoutGroup ON this rect (the controls page's three columns). layRect then places this
  // rect's children by Unity's LayoutGroup.SetChildrenAlongAxis instead of their own anchors.
  bool verticalLayout = false;
  float padL = 0, padR = 0, padT = 0, padB = 0, spacing = 0;
  int childAlignment = 0;              // TextAnchor: 0 UpperLeft .. 8 LowerRight
  bool forceExpandW = true, forceExpandH = true, controlW = false, controlH = false;
};

// uGUI CanvasScaler + the MaterialKit subclass (UiManager applies saved UiSize prefs)
struct CanvasScaler : public Component {
  Vec2 referenceResolution;            // Unity CanvasScaler.referenceResolution
  // A Canvas is laid out in DESIGN units and rasterised at scaleFactor pixels per unit. Without
  // these, the canvas was assumed to be the screen, which is only true for ConstantPixelSize --
  // School's Hud is ScaleWithScreenSize 640x480 Expand, i.e. a 853x480 design rect at 1280x720.
  int uiScaleMode = 0;                 // 0 ConstantPixelSize, 1 ScaleWithScreenSize, 2 ConstantPhysicalSize
  int screenMatchMode = 0;             // 0 MatchWidthOrHeight, 1 Expand, 2 Shrink  (Unity enum)
  float matchWidthOrHeight = 0.0f;     // 0 = match width, 1 = match height (mode 0 only)
  float scaleFactor = 1.0f;            // ConstantPixelSize multiplier
};
// MaterialKit DpCanvasScaler (iOS touch canvas): a screen-space canvas gets scaleFactor =
// Screen.dpi / 160 (m_FallbackScreenDPI when dpi is 0) -- iOS HandleConstantPhysicalSize @0x100aa4c40.
// Layout units are density-independent points, so the controls keep their physical size.
struct DpCanvasScaler : public CanvasScaler {
  float fallbackScreenDPI = 96.0f;
  float defaultSpriteDPI = 96.0f;
};

// Physics-lite placeholders (Phase 3 implements; enabled-toggles used by Teleporter etc.)
struct CharacterController : public Component {
  float minMoveDistance = 0.001f;      // Unity CharacterController.minMoveDistance (baked)
  float height = 2.0f;                 // capsule height (baked; raycast treats CC as a capsule)
  float radius = 0.5f;                 // capsule radius (baked)
  float skinWidth = 0.08f;             // Unity skinWidth (Move repel, Phase 3)
  Vec3 center;                         // local capsule center (baked)
  Vec3 velocity() const;             // engine impl (capsule mover)
  void Move(const Vec3& motion);     // engine impl (capsule mover)
};
struct Collider : public Component {              // shapes/triggers carried through RaycastHit
  bool isTrigger = false;            // Unity Collider.isTrigger (baked per collider entry)
};
// Unity mesh collider (the school's walls/floors/ceilings, plus the SwingingDoor barrier).
//
// Every MeshCollider in the 7 game scenes references one of exactly TWO Unity built-in primitive
// meshes, so no mesh vertex data is ever loaded: the loader classifies the collider from the mesh
// fileID and the raycast treats it as a flat rectangle. Both primitives are ZERO-THICKNESS
// surfaces, which is why they are not modelled as boxes -- an OBB with a degenerate extent fails
// the slab test. `surface` says which local plane the rectangle lies in; `halfU`/`halfV` are its
// half sizes along that plane's two axes, in pre-scale local units.
struct MeshCollider : public Collider {
  enum Surface {
    SurfaceNone = 0,   // unrecognised mesh -> raycast skips it (one-time Debug notice)
    SurfaceQuad,       // built-in Quad (fileID 10210): 1x1 in the XY plane, normal = local +Z
    SurfacePlane,      // built-in Plane (fileID 10209): 10x10 in the XZ plane, normal = local +Y
  };
  int surface = SurfaceNone;
  float halfU = 0.0f;                // Quad: X half-size (0.5)   Plane: X half-size (5)
  float halfV = 0.0f;                // Quad: Y half-size (0.5)   Plane: Z half-size (5)
  bool convex = false;               // Unity m_Convex: PhysX cooks a SOLID hull (see Raycast)
};
struct BoxCollider : public Collider {            // Unity box collider (faculty hitbox)
  Vec3 size = Vec3::one();           // Unity BoxCollider.size (local/pre-scale extents)
  Vec3 center;                       // local center
};
struct CapsuleCollider : public Collider {        // Unity capsule (1st Prize / notebooks)
  float radius = 0.5f;
  float height = 1.0f;
  int direction = 1;                 // Unity direction: 0 X, 1 Y, 2 Z
  Vec3 center;
};
// NavMesh agents (4 in game; plain SetDestination) -> Phase 4 grid A* + feel tuning
struct NavMeshAgent : public Component {
  Vec3 velocity() const;             // engine impl
  float speed = 0.0f;                // Unity agent.speed (Baldi only moves during slap frames)
  bool autoBraking = true;           // Unity default true; 1st Prize hugs run with it OFF
  void SetDestination(const Vec3& target); // engine impl
  void Warp(const Vec3& position);   // engine impl (Playtime backs off after touching the player)
  bool isStopped = false;            // Unity isStopped property ( Principal detentions stop him in place)
  void setVelocity(const Vec3& v);   // Unity NavMeshAgent.velocity property setter (BsodaEffect pushes)
  Vec3 steeringTarget() const;       // engine impl (next corner the agent steers toward)
  Vec3 destination() const;          // engine impl (current goal)
  // Serialized agent settings (all School agents: baseOffset 1, acceleration >= 1000).
  float baseOffset = 0.0f;           // pivot height above the navmesh surface
  float stoppingDistance = 0.0f;
  float angularSpeed = 120.0f;       // deg/s, updateRotation turns the transform toward its motion
  float acceleration = 8.0f;         // u/s^2 toward `speed` (1st Prize: 10; every other agent >= 1000)
};

// Unity Rigidbody (BSODA spray projectiles + the effect's velocity transfer). velocity is a real
// field here; engine Phase 3 integrates the straight-line motion.
struct Rigidbody : public Component {
  Vec3 velocity;
  // Serialized: every School NPC body is kinematic (moved by its agent); the BSODA spray's is
  // dynamic, gravity off, drag 0 -- the physics step moves it by `velocity` (eng-core.cpp).
  bool isKinematic = false;
  bool useGravity = true;
  float drag = 0.0f;
};

// Unity NavMeshObstacle (255 counter/hallway blockers; Phase 4 route planner consumes carving)
struct NavMeshObstacle : public Component {
  bool carving = false;
  Vec3 size = Vec3::one();
  Vec3 center;
};

// AudioClip gains the decoded PCM when its loadPcm() first runs (lazy, on first mixin).
// All 127 shipped clips are plain PCM16 (python RIFF scan), so one loader serves everything.
struct AudioClip : public Object {
  std::vector<int16_t> pcm;   // interleaved frames (mono: 1 value per frame)
  int rate = 0;               // sample rate (8000/22050/44100/48000)
  int channels = 0;           // 1 or 2
  bool pcmLoaded = false;
  void loadPcm();             // engine impl: full RIFF parse of the registered asset rel
};
// Pull-style output mixer, called by the shell's waveOut feed thread. dst receives n INTERLEAVED
// STEREO frames at 44100 Hz int16; every concurrently playing voice (AudioSim main + PlayOneShot
// shots) is linearly resampled (pitch = speed) and summed into dst (soft-clamped). advancing an
// internal sample clock; voices stamped with that clock at Play()/PlayOneShot() - see the
// AudioSim fields in eng-internal.h.
void audioPull(int16_t* dst, int frames);
void audioPullFloat(float* dst, int frames);   // same mix, unclamped float (Windows output)
struct AudioSource : public Component {
  AudioClip* clip = 0;
  bool loop = false;
  float volume = 1.0f;
  float pitch = 1.0f;                // 1st Prize motor whine follows speed * timeScale
  bool playOnAwake = false;          // Unity playOnAwake: plays when the source becomes active
  // Unity: a Play On Awake source starts when it becomes ACTIVE (not only at scene load) and stops
  // when its object is deactivated. Playtime's mus_Playtime and 1st Prize's 1PR_Motor loops are on
  // objects that are inactive at load and switched on by ActivateSpoopMode, so they never played.
  void OnEnable() override;          // engine impl
  void OnDisable() override;         // engine impl
  // Unity 3D audio fields, verbatim from the bakes. spatial = "Spatial Blend" as Unity serializes
  // it in this engine version (panLevelCustomCurve's first key value; 0 = 2D full volume, 1 = full
  // 3D distance rolloff). rolloff: 0 = Logarithmic, 1 = Linear, 2 = Custom (treated as Linear).
  // dist roll-off is applied in eng::audioPull against the listener (main camera) position cached
  // on the main thread, so the mix thread never touches scene state (see State::listenerPos).
  float spatial = 0.0f;
  float minDist = 1.0f;
  float maxDist = 500.0f;
  int rolloff = 0;
  bool mute = false;
  bb::Vec3 rollPos = bb::Vec3(0.0f, 0.0f, 0.0f); // cached transform->position copy for the mix thread
  bool isPlaying() const;            // engine impl
  void Play();                       // engine impl
  void Stop();                       // engine impl
  void Pause();                      // engine impl (TapePlayer pauses while Time.timeScale == 0)
  void UnPause();                    // engine impl
  void PlayOneShot(AudioClip* clip_, float volumeScale = 1.0f); // engine impl
};

struct Mathf {
  static int RoundToInt(float f) { return (int)(f >= 0 ? floorf(f + 0.5f) : ceilf(f - 0.5f)); } // half away from zero (Unity parity)
  static float Clamp(float v, float lo, float hi) { return clampf(v, lo, hi); }
  static float Max(float a, float b) { return a < b ? b : a; }
  // Unity pure-math pieces (exact formulas, inlined):
  static float Repeat(float t, float length) { return t - floorf(t / length) * length; } // t wraps into [0, length)
  static float DeltaAngle(float current, float target) { // shortest signed angle difference (degrees)
    float delta = Repeat(target - current, 360.0f);
    if (delta > 180.0f) delta -= 360.0f;
    return delta;
  }
  static float Abs(float f) { return f < 0.0f ? -f : f; }
  static float Atan2(float y, float x) { return atan2f(y, x); }
  static float Sign(float f) { return f >= 0.0f ? 1.0f : -1.0f; } // Unity: Sign(0) == 1
  static float Sin(float f) { return sinf(f); }
  static int CeilToInt(float f) { return (int)ceilf(f); }
};

// .NET System.Single.ToString() parity for strings the C# builds with string.Concat of floats
// (impossible-mode question numbers etc.) -- engine impl; format verified in the testbed phase.
std::string csharpToString(float f);

struct Random {
  static float range(float a, float b); // engine impl -- float: a..b inclusive
  static int   range(int a, int b);     // engine impl -- int: a..b-1
};

struct Screen {
  static int width();                   // engine impl (SDL testbed / UIView iOS)
  static int height();
};

// UnityEngine.Time -- engine-managed frame clock (50 Hz fixed step per the parity method)
struct Time {
  static float timeScale;               // Phase 2 .cpp definition; 1.0 running / 0.0 paused
  static int frameCount;                // Phase 2 .cpp definition; Unity Time.frameCount
  static float deltaTime();             // scaled (Update + coroutine ticks)
  static float unscaledDeltaTime();     // real-time (gameOverDelay UI fade)
};

struct Input {
  static bool anyKeyDown();             // engine impl (any key / touch / mouse-down)
  static bool anyKey();                 // engine impl: any key or mouse button HELD (Unity Input.anyKey)
  static bool GetKeyDown(KeyCode k);    // engine impl (edge-triggered; testbed keymap)
  static bool GetKey(KeyCode k);        // engine impl (HELD state; InputManager.cs GetKeyState)
  static bool GetKeyDown(const char* name); // Unity string overload ("return", "enter", ...)
  static bool GetButton(const char* button); // legacy named buttons ("Run" -- legacy PlayerMovement path)
  static float GetAxis(const char* axis); // engine impl (stock axis; "Mouse ScrollWheel" now)
  static bool GetMouseButtonDown(int button); // engine impl (edge-triggered; 0 = left; EndlessNotebook)
  static bool GetMouseButton(int button);     // engine impl (held state; Interact/UseItem default binding)
  static Vec3 mousePosition();  // engine impl (Unity: bottom-left-origin pixels of the client area;
                                // ClickableTest -> ScreenPointToRay; upper layers feed uiPointerMove)
};

// ---- Unity Cursor control (CursorControllerScript) ----
// Engine impl: SDL relative-mouse mode + hide/show on the Windows testbed; no-op on iOS
// (touch devices have no system cursor to lock).
enum class CursorLockMode : int { None = 0, Locked = 1, Confined = 2 };
struct Cursor {
  static CursorLockMode lockState;    // engine impl (testbed sets relative mode)
  static bool visible;                // engine impl
};

void QuitApplication();              // Application.Quit()

// Rewired (iOS build input path; engine impl: eng-rewired.cpp). The genuine iOS scripts read
// actions by name from ReInput.players.GetPlayer(0); actions are fed by the touch controls.
enum class AxisCoordinateMode : int { Absolute = 0, Relative = 1 };
struct RewiredPlayer {
  float GetAxis(const char* action) const;
  bool GetButton(const char* action) const;
  bool GetButtonDown(const char* action) const;
  AxisCoordinateMode GetAxisCoordinateMode(const char* action) const;
};
RewiredPlayer& ReInputPlayer();      // ReInput.players.GetPlayer(0)
bool ApplicationIsMobilePlatform();  // engine impl: the iOS build's code paths (eng::State::mobile)
// InputTypeManager.usingTouch (iOS, static): set on the first frame with a touch, cleared when a
// non-touch controller presses a button. MobileController shows the touch UI only while it is set.
bool InputTypeManagerUsingTouch();

// A Rewired touch component on an iOS canvas (TouchJoystick / TouchPad / TouchButton), carrying the
// genuine serialized settings; eng-rewired.cpp turns the live ones into the touch layout each frame.
struct RewiredTouchControl : public Component {
  enum Kind { Joystick = 0, Pad = 1, Button = 2 };
  int kind = Button;
  bool interactable = true;            // _interactable (false: a raycast blocker, e.g. Touch Pad_Dum)
  bool visible = true;                 // _visible (false: Rewired hides the control's own graphic)
  int elemX = -1, elemY = -1;          // _horizontal/_verticalAxisCustomControllerElement targets
  int elemButton = -1;                 // TouchButton _targetCustomControllerElement target
  // TouchButton _buttonType: 0 Standard (held while touched), 1 ToggleSwitch (each press flips the
  // value, release leaves it -- Rewired TouchButton press/release handlers). Run Button is 1.
  bool toggle = false;
  bool toggledOn = false;
  float stickRange = 60.0f;            // TouchJoystick _stickRange (canvas units)
  // TouchJoystick _stickTransform: the stick graphic. Rewired sets its anchoredPosition to the
  // original one + value * stickRange each time the value changes; release centres it
  // (_centerStickOnRelease 1). Engine-resolved; stickOrig is captured on first use.
  RectTransform* stick = 0;
  Vec2 stickOrig;
  bool stickOrigSet = false;
  // Graphics hidden while !visible: the target graphic + transitioner graphics, as paths relative
  // to this control ("" = itself) -- derived by tools/ios-bake-mobile.py from the genuine data.
  std::vector<std::string> hideOnInvisible;
  bool hiddenApplied = false;          // engine: the hide list has been applied to this instance
  // ColorTint transition targets (derived by tools/ios-bake-mobile.py from the genuine data):
  // graphic path relative to the control + its state colours. Engine-resolved Image + live tint.
  struct TintTarget {
    std::string path;
    Color normal, highlighted, pressed, disabled;
    float multiplier = 1.0f, fade = 0.1f;
    Image* image = 0;
    bool resolved = false, started = false;
    Color from, to;
    float t = 1.0f;
  };
  std::vector<TintTarget> tints;
};
void LoadScene(const std::string& name); // SceneManager.LoadScene (scene names from the manifest)
void LoadSceneAsync(const std::string& name); // SceneManager.LoadSceneAsync (math-game cheat -> TestRoom)
struct Debug {
  static void Log(const char* msg);   // engine impl (stdout on testbed / os_log on iOS)
};
void setAmbientLight(const Color& c);    // RenderSettings.ambientLight (finale red-light pass)
void setSkybox(Material* m);             // RenderSettings.skybox (Baldi catch -> black skybox)

// ---- -debug cheat menu (bottom-left; opened with the / key) --------------------------------
// The ENGINE renders the panel and routes clicks; the GAME (GameControllerScript, story/endless
// only) supplies the rows and the actions, one DebugUiSink frame call per Update. Available only
// while the host turned the debug UI on (the shell's -debug/--debug), which is why no gameplay
// code path ever sees it. While the panel is open the engine swallows the game's mouse-button
// reads so a player click inside the menu cannot fire a door/interact behind it.
struct DebugUiSink {
  virtual ~DebugUiSink() {}
  virtual int debugUiRows() = 0;                     // row count this frame (0 = no panel)
  virtual const char* debugUiRow(int i, bool* toggle, bool* on) = 0; // label + optional ON/OFF chip
  virtual void debugUiActivate(int i) = 0;           // row clicked
};
void debugUiFrame(DebugUiSink* sink, bool open);     // gc's Update, every frame while in game
bool debugUiAvailable();              // -debug given (hint/panel drawn only then)
bool cheatNpcIgnore();                // the "make NPCs ignore you" cheat (NPC scripts read)
void setCheatNpcIgnore(bool on);      // the menu's toggle for it
void debugUiEsp(bool on, const char* const* gameObjectNames, int count);
                                      // "ESP through walls": sprites of GameObjects whose root or
                                      // any ancestor is named in the list draw through walls

// C# IEnumerator -> per-frame state machine (one tick == one `yield return null`)
struct Coroutine {
  virtual ~Coroutine() {}
  virtual bool tick(float dt) = 0;    // dt == Time.deltaTime for that frame; false = finished
};
void StartCoroutine(Coroutine* c);    // engine registers + runs the first tick with the current
                                      // frame's dt (Unity starts enumerating synchronously)
GameObject* Instantiate(GameObject* prefab, const Vec3& pos, const Quat& rot);
GameObject* Instantiate(GameObject* prefab); // Unity Instantiate(original): prefab clones land at
                                             // (0,0,0) w/ identity rotation (NotebookScript's
                                             // learning-game spawn; engine impl Phase 2)

struct GameObject : public Object {
  int layer = 0;
  bool activeSelf = true;
  std::string tag = "Untagged";
  static GameObject* Find(const char* name); // engine impl (`GameObject.Find("...")`; scene-graph
                                             // name lookup -- EndlessNotebook wires gc/player)
  static GameObject* FindGameObjectWithTag(const char* tag); // engine impl (Unity FindGameObjectWithTag;
                                             // active objects only, first match -- ClickableTest)
  Transform* transform = 0;
  std::vector<Component*> components;  // attached components (scene loader / AddComponent)
  bool dontDestroy = false;            // DontDestroyOnLoad mark (survives scene swaps)

  template <class T> T* GetComponent() { return GetComponent_impl<T>(this); } // Unity GameObject.GetComponent
  template <class T> T* AddComponent(); // engine-defined (registers in the scene graph)
  // Unity: OnEnable / OnDisable run for every enabled component whose activeInHierarchy changes
  // (this object and its descendants). It used to flip the flag only, so a screen that was shown
  // again never re-ran its OnEnable (ControlsMenu refreshes its labels there; JumpRopeScript,
  // MenuController, UIController and OnAwakeTrigger all have one).
  void SetActive(bool v);              // engine impl
  // Unity GameObject.activeInHierarchy: this object AND every ancestor must be active. An object
  // whose own flag is true but whose parent is disabled does not render or tick in Unity, and the
  // shipped School scene relies on it -- `Old/PrototypeStuff` and a `Locker` under it are both
  // inactive, so the 252nd locker (the one in front of the player spawn) must not draw. Testing
  // activeSelf alone put a flat-red prototype locker in the player's face.
  bool activeInHierarchy() const;      // engine impl (walks transform parents)
  bool CompareTag(const std::string& t) const { return tag == t; }
};

} // namespace bb

// OUTSIDE bb: eng-impl.h re-opens the bb namespaces itself; keeping the include below the
// namespace close is what prevents eng-internal.h's `namespace eng` from nesting under bb.
#include "eng-impl.h"
