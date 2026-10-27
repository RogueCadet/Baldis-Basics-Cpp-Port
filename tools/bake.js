'use strict';
// bake.js  -  Phase 1 asset pipeline: scenes -> engine-load binding tables.
// Reads data/scene-<Name>.graph.json (output of scan.js), resolves script class names
// and asset GUIDs against data/manifest.json, and emits data/scene-<Name>.bake.json:
//   { name, roots[], objects[], components{fileID->entry}, assets{guid->info} }
// Component entries are class-specific flattened field sets (see SELECT below) so the
// Phase 2 scene loader binds C++ members directly. MonoBehaviours keep their serialized
// field block verbatim (minus Unity boilerplate)  -  the C++ structs use identical names.
//
// Field-selection conventions (engine-facing names, m_ prefix stripped):
//   refs:  { fileID, guid?, type? } kept as objects; in-object refs keep fileID only where
//          it is a scene-local id; asset refs must carry guid (fileID within normalized meta).
//   The scene loader (Phase 2) maps these onto bb::* members by name.

const fs = require('fs');
const path = require('path');

const DATA = path.join(__dirname, '..', 'data');
const manifest = JSON.parse(fs.readFileSync(path.join(DATA, 'manifest.json'), 'utf8'));
const guidMap = manifest.guidMap;

const GAME_SCENES = ['GameOver', 'MainMenu', 'Warning', 'Results', 'Secret', 'TestRoom', 'School'];

const BOILER = new Set([
  'm_ObjectHideFlags', 'm_CorrespondingSourceObject', 'm_PrefabInstance', 'm_PrefabAsset',
  'm_GameObject', 'm_EditorHideFlags', 'm_EditorClassIdentifier', 'm_Script', 'm_Name',
]);

// Unity-stock script GUIDs that live in the uGUI/TMP package (not under Assets/, so they are
// absent from the manifest). Scenes reference them as MonoBehaviours; the engine maps them to
// engine components (CanvasScaler exists; TMP labels bind as UiText).
// TextMeshPro ships a SEPARATE guid per class (unlike uGUI, which shares one assembly guid), so
// guid-level mapping is correct here -- but it has to be done per class. Verified: every m_Script
// using the TextMeshProUGUI guid below has fileID 11500000, i.e. one class per guid.
const BUILTIN_SCRIPTS = {
  f4688fdb7df04437aeb418b961361dc5: 'TextMeshProUGUI', // TMP label; m_text/fontAsset kept for the rasterizer
  // MathGame's keypad entry box. MathGameScript::playerAnswer points at this and dereferences it
  // in Start (`playerAnswer->text = ""`, ActivateInputField()), so leaving it unresolved would
  // null the field and crash the learning game on load.
  '2da0c512f12947e489f739169773d7ca': 'TMP_InputField',
};

// UnityEngine.UI (uGUI) assembly guid. A whole ASSEMBLY shares one guid, so the guid alone does
// NOT identify a class -- the m_Script *fileID* does. Mapping this guid to a single name (it was
// previously mapped wholesale to 'CanvasScaler') mislabelled all 15 uGUI classes used by the game,
// which silently broke every uGUI-typed script field (PlayerScript.staminaBar, the RawImage item
// slots, Button/Toggle refs) and stopped the loader from ever registering an EventSystem.
const UGUI_GUID = 'f70555f144d8491a825f0804e09c671c';

// m_Script fileID -> class, derived from each document's serialized field signature (the
// signatures are disjoint, so this identification is exact). Counts are occurrences across the 7
// game scenes.
const UGUI_CLASS_BY_FILEID = {
  '-765806418': 'Image',                 // 114x  m_Sprite + m_Type + m_FillMethod
  '1392445389': 'Button',                //  63x  m_OnClick
  '-98529514':  'RawImage',              //  33x  m_Texture + m_UVRect
  '1980459831': 'CanvasScaler',          //  23x  m_UiScaleMode + m_ReferenceResolution
  '1301386320': 'GraphicRaycaster',      //  16x  m_IgnoreReversedGraphics + m_BlockingObjects
  '2109663825': 'Toggle',                //  10x  toggleTransition + graphic
  '1077351063': 'StandaloneInputModule', //   6x  m_HorizontalAxis + m_SubmitButton
  '-619905303': 'EventSystem',           //   6x  m_FirstSelected + m_sendNavigationEvents
  '708705254':  'Text',                  //   3x  m_FontData + m_Text (uGUI Text, not TMP)
  '-113659843': 'Slider',                //   3x  m_FillRect + m_HandleRect
  '-2061169968':'Scrollbar',             //   1x  m_HandleRect + m_Direction (no m_FillRect)
  '1367256648': 'ScrollRect',            //   1x  m_Content + m_Viewport
  '-1184210157':'ToggleGroup',           //   2x  m_AllowSwitchOff
  '-1200242548':'Mask',                  //   1x  m_ShowMaskGraphic
  // Horizontal/VerticalLayoutGroup share their entire FIELD set, but this key is the m_Script
  // fileID -- the class's identity -- and 1297475563 is VerticalLayoutGroup. The data agrees: each of
  // the 3 (MainMenu KeyboardInputs / MouseInputs / InputLabels) holds 13 children 35 tall in a
  // 455-tall rect (13 x 35 = 455), stacked. They now drive real layout (the controls page).
  '1297475563': 'VerticalLayoutGroup',   // 3x  m_Spacing + m_ChildForceExpand*
  // Prefab-only (MathGame's TMP_InputField subtree):
  '1679637790': 'LayoutElement',         // m_MinWidth/m_PreferredWidth/m_FlexibleWidth/m_LayoutPriority
  // RectMask2D serializes NO fields of its own, so it has no signature to match -- identified by
  // position instead: it sits on the "Text Area" clipping child of a TMP_InputField, which is
  // exactly what TMP's input field prefab puts there.
  '-146154839': 'RectMask2D',
};

// Canonical fileID spelling for everything this baker emits: an exact decimal STRING, or null.
// Unity fileIDs exceed 2^53 (prefab ids reach ~9.06e18; MainMenu.unity carries 192 such ids), so
// unity-yaml hands back a number for the ones a JS double can hold exactly and a string for the
// rest. Normalising to string keeps them lossless end to end: the bake's object/component map KEYS
// are strings already, and the C++ loader reads ids with atoll into a long long.
// `fileID: 0` is Unity's null reference and stays the string "0".
function fid(v) {
  if (typeof v === 'string') return /^-?\d+$/.test(v) ? v : null;
  if (typeof v === 'number' && Number.isInteger(v)) return String(v);
  return null;
}

// ref normalizer: Unity stores asset refs as { fileID, guid, type }  -  fileID is the local
// id inside the asset file (textures: -1). Keep guid + fileID; drop type.
function refObj(v) {
  if (!v || typeof v !== 'object' || v.fileID === undefined) return null;
  const r = { fileID: fid(v.fileID) };
  if (v.guid && v.guid !== 0) r.guid = String(v.guid);
  return r;
}
// scene-local object ref (another component in this scene by fileID)
function localRef(v) {
  if (!v || typeof v !== 'object') return '0';
  const f = fid(v.fileID);
  if (f === null || f === '0') return '0';
  if (v.guid && v.guid !== 0) return null; // external  -  not scene-local (should not happen w/o prefabs)
  return f;
}
function vec3(v) { return v ? { x: +v.x || 0, y: +v.y || 0, z: +v.z || 0 } : { x: 0, y: 0, z: 0 }; }
function vec2(v) { return v ? { x: +v.x || 0, y: +v.y || 0 } : { x: 0, y: 0 }; }
function quat(v) { return v ? { x: +v.x || 0, y: +v.y || 0, z: +v.z || 0, w: v.w === undefined ? 1 : +v.w } : { x: 0, y: 0, z: 0, w: 1 }; }
function color(v) { return v ? { r: +v.r || 0, g: +v.g || 0, b: +v.b || 0, a: v.a === undefined ? 1 : +v.a } : { r: 1, g: 1, b: 1, a: 1 }; }

// ---- per-class selectors --------------------------------------------------
// key = source m_field, value = output name; FUNCTION values compute derived entries.
// Special names: {LIST: fn} for whole-block transforms.
const SELECT = {
  Transform: {
    m_LocalPosition: ['pos', vec3], m_LocalRotation: ['rot', quat], m_LocalScale: ['scale', vec3],
    m_Father: ['father', localRef],
    m_Children: ['children', (v) => (Array.isArray(v) ? v.map(localRef).filter((x) => x !== '0' && x !== null) : [])],
    m_RootOrder: ['rootOrder', (v) => v | 0],
  },
  MeshRenderer: {
    m_Enabled: ['enabled', (v) => !!v], m_Materials: ['materials', (v) => (Array.isArray(v) ? v.map(refObj).filter(Boolean) : [])],
    m_CastShadows: ['castShadows', (v) => v | 0], m_ReceiveShadows: ['receiveShadows', (v) => v | 0],
  },
  MeshFilter: { m_Mesh: ['mesh', refObj] },
  MeshCollider: {
    m_Enabled: ['enabled', (v) => !!v], m_IsTrigger: ['isTrigger', (v) => !!v], m_Convex: ['convex', (v) => !!v],
    m_Mesh: ['mesh', refObj], m_Material: ['material', refObj],
  },
  BoxCollider: {
    m_Enabled: ['enabled', (v) => !!v], m_IsTrigger: ['isTrigger', (v) => !!v],
    m_Size: ['size', vec3], m_Center: ['center', vec3], m_Material: ['material', refObj],
  },
  CapsuleCollider: {
    m_Enabled: ['enabled', (v) => !!v], m_IsTrigger: ['isTrigger', (v) => !!v],
    m_Radius: ['radius', (v) => +v], m_Height: ['height', (v) => +v], m_Direction: ['direction', (v) => v | 0],
    m_Center: ['center', vec3], m_Material: ['material', refObj],
  },
  NavMeshObstacle: {
    m_Enabled: ['enabled', (v) => !!v], m_Shape: ['shape', (v) => v | 0], m_Carving: ['carving', (v) => !!v],
    m_Size: ['size', vec3], m_Center: ['center', vec3],
  },
  SpriteRenderer: {
    m_Enabled: ['enabled', (v) => !!v], m_Sprite: ['sprite', refObj], m_Color: ['color', color],
    m_SortingLayerID: ['sortingLayer', (v) => v | 0], m_SortingOrder: ['sortingOrder', (v) => v | 0],
  },
  AudioSource: {
    m_Enabled: ['enabled', (v) => !!v], m_audioClip: ['clip', refObj],
    m_PlayOnAwake: ['playOnAwake', (v) => !!v], Loop: ['loop', (v) => !!v], Mute: ['mute', (v) => !!v],
    m_Volume: ['volume', (v) => +v], m_Pitch: ['pitch', (v) => +v],
    MinDistance: ['minDist', (v) => +v], MaxDistance: ['maxDist', (v) => +v],
    rolloffMode: ['rolloff', (v) => v | 0], Priority: ['priority', (v) => v | 0],
    Spatialize: ['spatialize', (v) => !!v], Pan2D: ['pan2d', (v) => +v],
    // Unity 5.x-2018 serializes "Spatial Blend" ONLY as the panLevelCustomCurve ("Pan Level"
    // animatable property): a flat unmoving curve whose first key value IS the blend (1 = full 3D).
    // There is no separate m_SpatialBlend line anywhere in these scene/prefab yamls (verified:
    // 0 hits across every scene), so the first key value is what the real engine samples.
    panLevelCustomCurve: ['panLevel', (v) => +(v && v.m_Curve && v.m_Curve[0] ? v.m_Curve[0].value : 0)],
  },
  Camera: {
    m_Enabled: ['enabled', (v) => !!v], m_ClearFlags: ['clearFlags', (v) => v | 0],
    m_BackGroundColor: ['bgColor', color],
    'near clip plane': ['near', (v) => +v], 'far clip plane': ['far', (v) => +v],
    'field of view': ['fov', (v) => +v], orthographic: ['ortho', (v) => !!v],
    'orthographic size': ['orthoSize', (v) => +v], m_Depth: ['depth', (v) => +v],
    m_CullingMask: ['cullingMask', (v) => (v && typeof v === 'object' ? (v.m_Bits | 0) : (v | 0))],
    m_NormalizedViewPortRect: ['viewport', (v) => ({ x: +v.x || 0, y: +v.y || 0, w: +v.width || 0, h: +v.height || 0 })],
    m_AllowMSAA: ['msaa', (v) => !!v], m_HDR: ['hdr', (v) => !!v],
  },
  AudioListener: { m_Enabled: ['enabled', (v) => !!v] },
  CharacterController: {
    m_Enabled: ['enabled', (v) => !!v], m_Height: ['height', (v) => +v], m_Radius: ['radius', (v) => +v],
    m_Center: ['center', vec3], m_SlopeLimit: ['slopeLimit', (v) => +v], m_StepOffset: ['stepOffset', (v) => +v],
    m_SkinWidth: ['skinWidth', (v) => +v], m_MinMoveDistance: ['minMoveDistance', (v) => +v],
  },
  Rigidbody: {
    m_Mass: ['mass', (v) => +v], m_Drag: ['drag', (v) => +v], m_AngularDrag: ['angularDrag', (v) => +v],
    m_UseGravity: ['useGravity', (v) => !!v], m_IsKinematic: ['isKinematic', (v) => !!v],
    m_Interpolate: ['interpolate', (v) => v | 0], m_Constraints: ['constraints', (v) => v | 0],
    m_CollisionDetection: ['collisionDetection', (v) => v | 0],
  },
  NavMeshAgent: {
    m_Enabled: ['enabled', (v) => !!v], m_Radius: ['radius', (v) => +v], m_Height: ['height', (v) => +v],
    m_Speed: ['speed', (v) => +v], m_Acceleration: ['acceleration', (v) => +v],
    m_AngularSpeed: ['angularSpeed', (v) => +v], m_StoppingDistance: ['stoppingDistance', (v) => +v],
    m_AutoBraking: ['autoBraking', (v) => !!v], avoidancePriority: ['avoidancePriority', (v) => v | 0],
    m_ObstacleAvoidanceType: ['obstacleAvoidanceType', (v) => v | 0], m_BaseOffset: ['baseOffset', (v) => +v],
  },
  Animator: {
    m_Enabled: ['enabled', (v) => !!v], m_Controller: ['controller', refObj], m_Avatar: ['avatar', refObj],
    m_CullingMode: ['cullingMode', (v) => v | 0],
    // 0 Normal, 1 AnimatePhysics, 2 UnscaledTime. BaldiFeed (the ThinkPad face), the pause menu's
    // Baldi nod/shake buttons and MainMenu's loading animation are 2: they animate while
    // Time.timeScale is 0, which is exactly when they are on screen.
    m_UpdateMode: ['updateMode', (v) => v | 0],
  },
  Canvas: {
    m_Enabled: ['enabled', (v) => !!v], m_RenderMode: ['renderMode', (v) => v | 0],
    m_Camera: ['camera', localRef], 'm_SortingLayerID': ['sortingLayer', (v) => v | 0],
    m_SortingOrder: ['sortingOrder', (v) => v | 0],
  },
  RectTransform: {
    // RectTransform is a Transform  -  Unity serializes the base fields again on it.
    m_LocalPosition: ['pos', vec3], m_LocalRotation: ['rot', quat], m_LocalScale: ['scale', vec3],
    m_Father: ['father', localRef],
    m_Children: ['children', (v) => (Array.isArray(v) ? v.map(localRef).filter((x) => x !== '0' && x !== null) : [])],
    m_RootOrder: ['rootOrder', (v) => v | 0],
    m_AnchorMin: ['anchorMin', vec2], m_AnchorMax: ['anchorMax', vec2],
    m_AnchoredPosition: ['anchoredPosition', vec2], m_SizeDelta: ['sizeDelta', vec2],
    m_Pivot: ['pivot', vec2],
  },
  CanvasRenderer: {}, // raw pass-through renderer (UI draw target; engine UI layer re-uses meshes)
};

const kindOf = (ext) => {
  switch (ext) {
    case '.wav': case '.ogg': case '.mp3': return 'audio';
    case '.png': case '.jpg': case '.jpeg': case '.tga': case '.psd': return 'texture';
    case '.fbx': case '.obj': case '.blend': return 'mesh';
    case '.mat': return 'material';
    case '.ttf': case '.otf': return 'font';
    case '.unity': return 'scene';
    case '.cs': return 'script';
    case '.controller': return 'animatorController';
    case '.anim': return 'animClip';
    case '.renderTexture': return 'renderTexture';
    case '.spriteatlas': return 'spriteAtlas';
    case '.cubemap': return 'cubemap';
    default: return 'other';
  }
};

// Bake targets. Prefabs go through the EXACT same pipeline as scenes: a .prefab carries the same
// GameObject/Transform/component documents, so the whole body below is kind-agnostic and the
// Phase 2 loader can instantiate a prefab bake with the same passes it uses for a scene.
// Prefab targets are discovered from whatever scan.js emitted (PrefabInstance/ only).
const targets = GAME_SCENES.map((name) => ({ kind: 'scene', name }));
for (const f of fs.readdirSync(DATA).sort()) {
  const m = /^prefab-(.+)\.graph\.json$/.exec(f);
  if (m) targets.push({ kind: 'prefab', name: m[1] });
}

const totals = { assets: {}, scenes: {}, prefabs: {} };
for (const { kind, name } of targets) {
  const g = JSON.parse(fs.readFileSync(path.join(DATA, `${kind}-${name}.graph.json`), 'utf8'));
  const assets = {};
  const noteAsset = (ref, via, kindOverride) => {
    if (!ref || !ref.guid) return;
    const gm = guidMap[ref.guid];
    let kind;
    if (ref.guid.startsWith('0000000000000000f0')) kind = 'builtin'; // Unity engine built-ins (no file on disk)
    else kind = kindOverride || kindOf(gm ? gm.ext : '');
    const key = gm ? gm.rel : `<builtin:${ref.guid.slice(16)}>`.replace('<builtin:f0', 'f0').replace('0000000000000000f0', 'builtin');
    const a = assets[key] || (assets[key] = { guid: ref.guid, kind, refs: {}, via: {} });
    a.refs[ref.fileID] = (a.refs[ref.fileID] || 0) + 1;
    a.via[via] = (a.via[via] || 0) + 1;
  };

  const objects = {};
  const components = {};
  const roots = [];
  const scriptMissing = [];
  for (const id of Object.keys(g.objects)) {
    const o = g.objects[id];
    // comps entries stay exact fileID STRINGS (never `| 0`, which both truncates to 32 bits and
    // destroys ids past 2^53).
    objects[id] = {
      n: o.n, tag: o.tag, layer: o.layer | 0, active: !!o.active,
      comps: o.comps.map(fid).filter((c) => c !== null && c !== '0'),
    };
    if (o.staticFlags) objects[id].staticFlags = o.staticFlags | 0;
  }

  for (const cid of Object.keys(g.components)) {
    const c = g.components[cid];
    const f = c.f || {};
    const entry = { t: c.t, go: c.go };
    const sel = SELECT[c.t];

    if (c.t === 'MonoBehaviour') {
      // resolve the script class from m_Script guid
      const sid = f.m_Script && f.m_Script.guid;
      const sfid = f.m_Script && f.m_Script.fileID;
      const gm = sid ? guidMap[sid] : null;
      let className;
      if (gm) {
        className = path.posix.basename(gm.rel, '.cs');     // a real Assets/Scripts class
      } else if (sid === UGUI_GUID) {
        // assembly-level guid: the fileID picks the class out of UnityEngine.UI
        className = UGUI_CLASS_BY_FILEID[String(sfid)] || `<unknown-ugui:${sfid}>`;
      } else if (sid) {
        className = BUILTIN_SCRIPTS[sid] || `<unknown:${sid}>`;
      } else {
        className = '<unknown>';
      }
      entry.t = 'Script';
      entry.script = className;
      const fields = {};
      for (const k of Object.keys(f)) {
        if (BOILER.has(k)) continue;
        let v = f[k];
        noteGuidsDeep(v, k);
        fields[k] = stripOnly(v, 0);
      }
      entry.f = fields;
      // "unresolved" = the class name could not be determined at all. A uGUI fileID that IS in the
      // table is resolved; one that is not shows up here as <unknown-ugui:...>.
      if (className.startsWith('<unknown')) scriptMissing.push(className);
    } else if (sel) {
      for (const k of Object.keys(sel)) {
        const [out, conv] = sel[k];
        let v = f[k];
        if (v === undefined) continue;
        let val = typeof conv === 'function' ? conv(v) : v;
        entry[out] = val;
        if ((out === 'materials' || out === 'mesh' || out === 'sprite' || out === 'clip' || out === 'material' || out === 'controller')) {
          const kid = (out === 'mesh') ? 'mesh' : undefined; // .asset mesh files: class context beats extension
          if (Array.isArray(val)) val.forEach((r) => noteAsset(r, c.t, kid));
          else if (val && val.guid) noteAsset(val, c.t, kid);
        }
      }
    }
    components[cid] = entry;
  }

  function noteGuidsDeep(v, via) {
    if (v == null || typeof v !== 'object') {
      if (typeof v === 'object') return;
      return;
    }
    if (Array.isArray(v)) { for (const x of v) noteGuidsDeep(x, via); return; }
    if (typeof v.fileID === 'number' && typeof v.guid === 'string' && v.guid) noteAsset(v, 'MonoBehaviour');
    for (const k of Object.keys(v)) noteGuidsDeep(v[k], via);
  }
  function stripOnly(v, depth) { // arrays are NOT collapsed in bakes (bindings need exact slots)
    if (depth > 24) return { __cut: 'depth' };
    if (Array.isArray(v)) return v.map((x) => stripOnly(x, depth + 1));
    if (v && typeof v === 'object') {
      const o = {};
      for (const k of Object.keys(v)) o[k] = stripOnly(v[k], depth + 1);
      return o;
    }
    return v;
  }

  // roots: Transforms with father == 0, in doc order (Unity instantiates first-seen first)
  // `father` and `go` are fileID strings now, so the null-parent test compares against "0" and
  // roots are emitted as strings (`id | 0` would truncate ids past 2^31/2^53).
  const isRoot = {};
  for (const cid of Object.keys(components)) {
    const c = components[cid];
    if ((c.t === 'Transform' || c.t === 'RectTransform') && c.father === '0') {
      isRoot[c.go] = true;
    }
  }
  for (const id of Object.keys(objects)) if (isRoot[id]) roots.push(id);

  // RenderSettings.skybox (graph.settings, from scan.js): the scene's configured RenderSettings.
  // m_SkyboxMaterial guid. Unity's built-in default material (0000000000000000f0...) is NOT a
  // rendered skybox, so it is dropped here: the engine keeps its clear colour for those scenes,
  // exactly as Unity's default renderer does.
  const skybox = g.settings && g.settings.skybox &&
    !g.settings.skybox.startsWith('0000000000000000f0') ? g.settings.skybox : null;
  const bake = skybox ? { name, skybox, roots, objects, components, assets }
                      : { name, roots, objects, components, assets };
  const outPath = path.join(DATA, `${kind}-${name}.bake.json`);
  fs.writeFileSync(outPath, JSON.stringify(bake));

  // summary
  const compByType = {};
  for (const cid of Object.keys(components)) { const t = components[cid].t; compByType[t] = (compByType[t] || 0) + 1; }
  const assetKinds = {};
  for (const rel of Object.keys(assets)) { const k = assets[rel].kind || 'other'; assetKinds[k] = (assetKinds[k] || 0) + 1; }
  for (const [rel, a] of Object.entries(assets)) totals.assets[`${rel} [${a.kind}]`] = 1;
  (kind === 'scene' ? totals.scenes : totals.prefabs)[name] = {
    bytes: fs.statSync(outPath).size, roots: roots.length,
    objects: Object.keys(objects).length, components: Object.keys(components).length,
    scripts: compByType.Script || 0, unresolved: scriptMissing.length,
    assetKinds,
  };
}

fs.writeFileSync(path.join(DATA, 'bake-summary.json'),
  JSON.stringify({ scenes: totals.scenes, prefabs: totals.prefabs }, null, 1));
const row = (n, s) => console.log(`${n.padEnd(32)} roots ${String(s.roots).padStart(3)}  objects ${String(s.objects).padStart(5)}  comps ${String(s.components).padStart(6)}  scripts ${String(s.scripts).padStart(3)}  unresolved ${s.unresolved}  ${(s.bytes / 1024).toFixed(0)} KB  kinds ${JSON.stringify(s.assetKinds)}`);
console.log('-- scenes --');
for (const [n, s] of Object.entries(totals.scenes)) row(n, s);
console.log('-- prefabs --');
for (const [n, s] of Object.entries(totals.prefabs)) row(n, s);
console.log('bake done');
