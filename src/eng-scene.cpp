// eng-scene.cpp ??? bake scene loader (Phase 2): instantiates data/scene-<Name>.bake.json into
// engine objects, computes Unity world transforms, binds script fields (bindings.cpp contract),
// runs the Unity load lifecycle (Awake -> OnEnable per active object; Start sweep), and owns
// scene swaps + unload semantics.
//
// Known Phase 2 approximations (documented debt):
//  - Prefab fields resolve to named template GameObject shells (shallow Instantiate; deep clones
//    with component sets land with the prefab loader in Phase 3).
//  - RectTransform UI world positions compose parent chains like Transforms (no anchor math).
//  - Material/Sprite/Texture nodes are identity-only (no pixels) ??? scripts test pointer identity.
//  - MeshColliders register but raycast-skip until the Phase 3 mesh bounds arrive.
//  - The CC shell CapsuleCollider mirrors Unity's PhysX capsule; it lives outside go->components
//    so GetComponent<CapsuleCollider>() still returns null like Unity does for CharacterController.

// Must precede every include: MSVC flags getenv (the ENG_TRACE_LOAD switch) as C4996 under /W3,
// and the suppression only takes effect if defined before the CRT headers are first pulled in.
#define _CRT_SECURE_NO_WARNINGS

#include "eng-internal.h"
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <typeinfo>   // ENG_TRACE_LOAD names components by dynamic type

namespace eng {

using bb::Vec2;
using bb::Vec3;
using bb::Quat;
using bb::Color;   // uGUI Image tint (newUguiScript)

// ---- asset manifest cache (guid -> rel path under Assets/) -----------------------------------
struct ManifestCache {
  bool loaded = false;
  std::unordered_map<std::string, std::string> relByGuid;
};
static ManifestCache& manifest() {
  static ManifestCache m;
  return m;
}

static void loadManifest() {
  ManifestCache& m = manifest();
  if (m.loaded) return;
  m.loaded = true;
  bool ok = false;
  const std::string text = jsonReadFile("data/manifest.json", ok);
  if (!ok) return;
  JsonValue root;
  std::string err;
  if (!jsonParse(text, root, err)) return;
  const JsonValue* gm = root.find("guidMap");
  if (!gm) return;
  for (size_t i = 0; i < gm->obj.size(); i++) {
    const JsonValue* rel = gm->obj[i].second.find("rel");
    if (rel && rel->type == JsonValue::String) m.relByGuid[gm->obj[i].first] = rel->str;
  }
}

static std::string relForGuid(const std::string& guid) {
  loadManifest();
  std::unordered_map<std::string, std::string>::iterator it = manifest().relByGuid.find(guid);
  return it != manifest().relByGuid.end() ? it->second : std::string(guid);
}

// Public form for eng-core (clipForGuid): unlike relForGuid above it reports failure as an EMPTY
// string rather than echoing the guid back, so callers can tell "no such asset" from a real path.
std::string assetRelForGuid(const std::string& guid) {
  loadManifest();
  std::unordered_map<std::string, std::string>::iterator it = manifest().relByGuid.find(guid);
  return it != manifest().relByGuid.end() ? it->second : std::string();
}

// ---- TMP fonts (font-bake.js output: data/font-<Name>.json [.atlas] + fonts-index.json) -------
// Data shape (all keys authored by the tool): face metrics + per-glyph records carrying FINAL
// atlas UVs (the tool resolved the asset's y convention against atlas ink) + xo/yo/adv in baked
// point-size units. Persistent registry: fonts never die with a scene (like materials).
static TMPFontData* loadFontBake(const std::string& name) {
  State& g = state();
  TMPFontData& f = g.tmpFonts[name];   // registered even on a missing bake (advance-only font)
  f.name = name;
  bool ok = false;
  // File name rule = font-bake.js: whitespace -> '_' ("LiberationSans SDF" is written as
  // font-LiberationSans_SDF.json). Opening "font-LiberationSans SDF.json" failed silently, so every
  // font with a space in its name -- TMP's default LiberationSans SDF among them, 55 MainMenu
  // labels -- loaded with no glyph table and no atlas.
  std::string fileName = name;
  for (size_t i = 0; i < fileName.size(); i++) if (fileName[i] == ' ' || fileName[i] == '	') fileName[i] = '_';
  const std::string text = jsonReadFile("data/font-" + fileName + ".json", ok);
  if (!ok) return &f;
  JsonValue j;
  std::string err;
  if (!jsonParse(text, j, err)) return &f;
  const JsonValue* vn = j.find("name");
  if (vn && vn->type == JsonValue::String) f.name = vn->asString();
  f.pointSize = j.find("pointSize") ? (float)j.find("pointSize")->numberOr(24.0) : 24.0f;
  f.lineHeight = j.find("lineHeight") ? (float)j.find("lineHeight")->numberOr(33.0) : 33.0f;
  f.ascender = j.find("ascender") ? (float)j.find("ascender")->numberOr(26.0) : 26.0f;
  f.descender = j.find("descender") ? (float)j.find("descender")->numberOr(-7.0) : -7.0f;
  f.scale = j.find("scale") ? (float)j.find("scale")->numberOr(1.0) : 1.0f;
  f.sdf = j.find("sdf") ? j.find("sdf")->asBool(false) : false;
  f.atlasW = j.find("atlasW") ? j.find("atlasW")->asInt(0) : 0;
  f.atlasH = j.find("atlasH") ? j.find("atlasH")->asInt(0) : 0;
  const JsonValue* atlas = j.find("atlas");
  if (atlas && atlas->type == JsonValue::String && f.rgba.empty()) {
    if (f.rgba.empty()) {
      const std::string blob = jsonReadFile(atlas->asString(), ok);
      if (ok && (int)blob.size() == f.atlasW * f.atlasH * 4) {
        f.rgba.assign(blob.begin(), blob.end());
      }
    }
  }
  const JsonValue* glyphs = j.find("glyphs");
  if (glyphs && glyphs->type == JsonValue::Array) {
    for (size_t i = 0; i < glyphs->arr.size(); i++) {
      const JsonValue& gj = glyphs->arr[i];
      TMPFontGlyph gl;
      gl.u0 = gj.find("u0") ? gj.find("u0")->asFloat() : 0.0f;
      gl.v0 = gj.find("v0") ? gj.find("v0")->asFloat() : 0.0f;
      gl.u1 = gj.find("u1") ? gj.find("u1")->asFloat() : 0.0f;
      gl.v1 = gj.find("v1") ? gj.find("v1")->asFloat() : 0.0f;
      gl.w = gj.find("w") ? gj.find("w")->asFloat() : 0.0f;
      gl.h = gj.find("h") ? gj.find("h")->asFloat() : 0.0f;
      gl.xo = gj.find("xo") ? gj.find("xo")->asFloat() : 0.0f;
      gl.yo = gj.find("yo") ? gj.find("yo")->asFloat() : 0.0f;
      gl.adv = gj.find("adv") ? gj.find("adv")->asFloat() : 0.0f;
      f.glyphs[gj.find("id") ? gj.find("id")->asInt(0) : 0] = gl;
    }
  }
  return &f;
}

// TMP m_text escape decoding (this layer's version of TMP's char-parse rules). TMP's text
// parser processes "\uXXXX" escape sequences INSIDE the text value at composition time -- the
// game ships "\u200B" (zero-width space) as the InputField's "empty" marker, so without the
// decode those six ASCII characters draw literally. We decode \uXXXX (TMP also accepts the
// literal char in most assets; the escapes are what the scenes carry) and:
//  - keep '\n'/'\t' as the real control chars they already are,
//  - DROP characters that draw as nothing in TMP with zero advance when we carry no glyph for
//    them at all: codepoints < 0x20 (except \n/\t), BOM/word-joiner (FEFF/2060), and the
//    U+2000..U+200F invisible run (200B zero-width space lives there).
std::string decodeTmpEscapes(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); i++) {
    if (in[i] == '\\' && i + 5 < in.size() && in[i + 1] == 'u') {
      int v = 0, digits = 0;
      for (int k = 0; k < 4; k++) {
        const char c = in[i + 2 + k];
        const int d = (c >= '0' && c <= '9') ? c - '0'
                    : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                    : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) break;
        v = v * 16 + d;
        digits++;
      }
      if (digits == 4) {
        i += 5;
        const int cp = v;
        if (cp == 0x200B || (cp >= 0x2000 && cp <= 0x200F) || cp == 0xFEFF || cp == 0x2060) {
          continue;   // zero-width in TMP / nothing to draw: drop entirely
        }
        if (cp < 0x20) continue;   // stray control char: TMP renders nothing, advance 0
        // encode UTF-8 (surrogate halves never reach here: \uXXXX pairs map 1:1 in this game)
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) {
          out += (char)(0xC0 | (cp >> 6));
          out += (char)(0x80 | (cp & 0x3F));
        } else {
          out += (char)(0xE0 | (cp >> 12));
          out += (char)(0x80 | ((cp >> 6) & 0x3F));
          out += (char)(0x80 | (cp & 0x3F));
        }
        continue;
      }
    }
    out += in[i];
  }
  return out;
}

TMPFontData* fontForGuid(const std::string& guid) {
  State& g = state();
  if (!g.fontsIndexLoaded) {
    g.fontsIndexLoaded = true;
    bool ok = false;
    const std::string text = jsonReadFile("data/fonts-index.json", ok);
    if (ok) {
      JsonValue idx;
      std::string err;
      if (jsonParse(text, idx, err) && idx.type == JsonValue::Object) {
        for (size_t i = 0; i < idx.obj.size(); i++) {
          if (idx.obj[i].second.type == JsonValue::String) {
            g.fontGuidToName[idx.obj[i].first] = idx.obj[i].second.asString();
          }
        }
      }
    }
  }
  std::map<std::string, std::string>::const_iterator it = g.fontGuidToName.find(guid);
  if (it == g.fontGuidToName.end()) return 0;
  std::map<std::string, TMPFontData>::iterator fit = g.tmpFonts.find(it->second);
  if (fit != g.tmpFonts.end()) return &fit->second;
  return loadFontBake(it->second);
}

// material guid -> main texture guid (data/material-textures.json, from
// tools/bake-material-textures.js). A MeshRenderer references a MATERIAL, not a texture, so the
// world render pass needs this hop to reach pixels. Returns "" for flat-colour materials and for
// Unity's built-in default material, which has no asset file at all.
struct MatTexInfo {
  std::string tex;
  float sx, sy, ox, oy;
  float cr, cg, cb, ca;
  bool at;                                     // material sets _ALPHATEST_ON (cutout)
  MatTexInfo() : sx(1), sy(1), ox(0), oy(0), cr(1), cg(1), cb(1), ca(1), at(false) {}
};
static std::unordered_map<std::string, MatTexInfo>& matTexTable();

static std::string materialMainTexture(const std::string& materialGuid) {
  std::unordered_map<std::string, MatTexInfo>::iterator it = matTexTable().find(materialGuid);
  return it != matTexTable().end() ? it->second.tex : std::string();
}

// The tiling lives in the same table; a separate accessor keeps the existing call site untouched.
static void materialTiling(const std::string& materialGuid, bb::Vec2* scale, bb::Vec2* offset) {
  std::unordered_map<std::string, MatTexInfo>::iterator it = matTexTable().find(materialGuid);
  if (it == matTexTable().end()) return;                 // defaults (1,1)/(0,0) already set
  if (scale) *scale = bb::Vec2(it->second.sx, it->second.sy);
  if (offset) *offset = bb::Vec2(it->second.ox, it->second.oy);
}

// The cutout flag comes from the same sweep: the material's m_ShaderKeywords carries
// _ALPHATEST_ON (11 game materials: Window, Dark_Window, the door materials, fence). The world
// pass alpha-tests those draws, so a window pane no longer paints its transparent texels solid
// over the room behind it.
static void materialAlphaTest(const std::string& materialGuid, bb::Material* m) {
  std::unordered_map<std::string, MatTexInfo>::iterator it = matTexTable().find(materialGuid);
  if (it == matTexTable().end() || !m) return;           // default false already set
  m->alphaTest = it->second.at;
}

static void materialColor(const std::string& materialGuid, bb::Material* m) {
  std::unordered_map<std::string, MatTexInfo>::iterator it = matTexTable().find(materialGuid);
  if (it == matTexTable().end() || !m) return;           // default white already set
  m->colorR = it->second.cr;
  m->colorG = it->second.cg;
  m->colorB = it->second.cb;
  m->colorA = it->second.ca;
}

static std::unordered_map<std::string, MatTexInfo>& matTexTable() {
  static std::unordered_map<std::string, MatTexInfo> texByMat;
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    bool ok = false;
    const std::string text = jsonReadFile("data/material-textures.json", ok);
    if (ok) {
      JsonValue root;
      std::string err;
      if (jsonParse(text, root, err) && root.type == JsonValue::Object) {
        for (size_t i = 0; i < root.obj.size(); i++) {
          const JsonValue& e = root.obj[i].second;
          // UNTEXTURED materials are kept too: skipping them lost their colour, so BSODA_Side
          // (the vending machines' sides, _Color 0.086 grey, no _MainTex) and BlackCeiling (black)
          // drew the renderer's light-grey placeholder instead.
          const JsonValue* tex = e.find("tex");
          MatTexInfo info;
          info.tex = (tex && tex->type == JsonValue::String) ? tex->str : std::string();
          const JsonValue* sx = e.find("scaleX");
          const JsonValue* sy = e.find("scaleY");
          const JsonValue* ox = e.find("offsetX");
          const JsonValue* oy = e.find("offsetY");
          if (sx) info.sx = sx->asFloat(1.0f);
          if (sy) info.sy = sy->asFloat(1.0f);
          if (ox) info.ox = ox->asFloat(0.0f);
          if (oy) info.oy = oy->asFloat(0.0f);
          const JsonValue* cr = e.find("colorR");
          const JsonValue* cg = e.find("colorG");
          const JsonValue* cb = e.find("colorB");
          const JsonValue* ca = e.find("colorA");
          if (cr) info.cr = cr->asFloat(1.0f);
          if (cg) info.cg = cg->asFloat(1.0f);
          if (cb) info.cb = cb->asFloat(1.0f);
          if (ca) info.ca = ca->asFloat(1.0f);
          const JsonValue* at = e.find("alphaTest");
          if (at) info.at = at->type == JsonValue::Bool ? at->b : (at->asFloat(0.0f) != 0.0f);
          texByMat[root.obj[i].first] = info;
        }
      }
    }
  }
  return texByMat;
}

static std::string basenameOf(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  const std::string file = (slash == std::string::npos) ? path : path.substr(slash + 1);
  return file.substr(0, file.find_last_of('.')); // strip extension
}

// ---- BindCtx asset factories (identity nodes; engineShutdown frees them) ---------------------
bb::GameObject* BindCtx::prefabFor(const std::string& guid, const std::string& rel) {
  State& g = state();
  std::unordered_map<std::string, bb::GameObject*>::iterator it = g.prefabTemplates.find(guid);
  if (it != g.prefabTemplates.end()) return it->second;
  const std::string path = rel.empty() ? relForGuid(guid) : rel;
  bb::GameObject* go = new bb::GameObject();
  go->name = basenameOf(path);
  noteRuntimeObject(go); // owner registry only ??? NOT the scene registry (Find must not see it)
  g.prefabTemplates[guid] = go;
  return go;
}

bb::Material* BindCtx::materialFor(const std::string& guid) {
  State& g = state();
  std::unordered_map<std::string, bb::Material*>::iterator it = g.materials.find(guid);
  if (it != g.materials.end()) return it->second;
  bb::Material* m = new bb::Material();
  m->name = guid;
  // FILL IT HERE, for every caller. This used to happen only on the MeshRenderer bind path, so a
  // material first reached through a SCRIPT FIELD stayed bare -- no texture, tint or cutout. The
  // open-door materials (Door_80, FacultyDoor_80, SwingDoor60[_Dark]: Standard, Rendering Mode
  // Cutout, _ALPHATEST_ON) are referenced ONLY by DoorScript / SwingingDoorScript fields and
  // swapped onto the faces at runtime, so an opening door drew solid instead of see-through.
  m->mainTexGuid = materialMainTexture(guid);
  materialTiling(guid, &m->texScale, &m->texOffset);
  materialColor(guid, m);
  materialAlphaTest(guid, m);
  noteRuntimeObject(m);
  g.materials[guid] = m;
  return m;
}

bb::Sprite* BindCtx::spriteFor(const std::string& guid) {
  State& g = state();
  std::unordered_map<std::string, bb::Sprite*>::iterator it = g.sprites.find(guid);
  if (it != g.sprites.end()) return it->second;
  bb::Sprite* s = new bb::Sprite();
  s->name = guid;
  noteRuntimeObject(s);
  g.sprites[guid] = s;
  return s;
}

bb::Texture* BindCtx::textureFor(const std::string& guid) {
  State& g = state();
  std::unordered_map<std::string, bb::Texture*>::iterator it = g.textures.find(guid);
  if (it != g.textures.end()) return it->second;
  bb::Texture* t = new bb::Texture();
  t->name = guid;
  noteRuntimeObject(t);
  g.textures[guid] = t;
  return t;
}

// ---- scene state ------------------------------------------------------------------------------
static std::string s_currentScene;
std::string currentSceneName() { return s_currentScene; }

// RenderSettings.m_SkyboxMaterial of the LIVE scene ('' = no configured skybox, keep the clear
// colour). Set from the bake's top-level "skybox" key in loadScene, cleared by unloadScene --
// scene state, not asset state: swapping to a scene without a configured skybox must not show
// the previous scene's sky.
static std::string s_currentSkybox;
const std::string& currentSceneSkybox() { return s_currentSkybox; }

// ENG_TRACE_LOAD=1 narrates the bind/Awake/Start passes to stdout. Load-time faults in scripts
// are otherwise opaque (no debugger on this box): the last line printed names the component.
static const bool s_traceLoad = getenv("ENG_TRACE_LOAD") != 0;

// scripts trimmed/excluded from the port (charter); their comps are skipped silently
static bool isTrimmedScript(const std::string& name) {
  return name == "NavMeshAgentRay" || name == "CameraScript_Simple" || name == "ClickableTest" ||
         name == "AgentTest" || name == "DebugScript";   // ControlsMenu is ported now (game/ControlsMenu.hpp)
}

// ---- small readers -----------------------------------------------------------------------------
static Vec3 vOf(const JsonValue* v) {
  if (!v || v->type != JsonValue::Object) return Vec3::zero();
  const JsonValue* x = v->find("x");
  const JsonValue* y = v->find("y");
  const JsonValue* z = v->find("z");
  return Vec3(x ? x->asFloat() : 0.0f, y ? y->asFloat() : 0.0f, z ? z->asFloat() : 0.0f);
}
static Vec2 v2Of(const JsonValue* v) {
  if (!v || v->type != JsonValue::Object) return Vec2(0.0f, 0.0f);
  const JsonValue* x = v->find("x");
  const JsonValue* y = v->find("y");
  return Vec2(x ? x->asFloat() : 0.0f, y ? y->asFloat() : 0.0f);
}
static Quat qOf(const JsonValue* v) {
  if (!v || v->type != JsonValue::Object) return Quat::identity();
  const JsonValue* x = v->find("x");
  const JsonValue* y = v->find("y");
  const JsonValue* z = v->find("z");
  const JsonValue* w = v->find("w");
  return Quat(x ? x->asFloat() : 0.0f, y ? y->asFloat() : 0.0f,
              z ? z->asFloat() : 0.0f, w ? w->asFloat() : 1.0f);
}
static bool boolOf(const JsonValue* v, bool def) {
  if (!v || v->type == JsonValue::Null) return def; // absent or null keeps the default
  return v->asBool(def);
}
static float floatOf(const JsonValue* v, float def) {
  if (!v || v->type == JsonValue::Null) return def;
  return v->asFloat(def);
}
static int intOf(const JsonValue* v, int def) {
  if (!v || v->type == JsonValue::Null) return def;
  return v->asInt(def);
}

// ---- Transform world compose -------------------------------------------------------------------
struct TEntry {                      // one Transform/RectTransform comp (world compose source)
  bb::Transform* t;
  bb::RectTransform* rt;             // non-null when the bake carries a RectTransform
  bb::GameObject* go;
  long long fid;
  Vec3 lp;                           // local position
  Quat lr;                           // local rotation
  Vec3 ls;                           // local scale
  long long father;                  // parent Transform comp fileID (0 = root)
  Quat wr;                           // world rotation (compose result)
  Vec3 wp;                           // world position (compose result; Unity position semantics)
  Vec3 ws;                           // world scale (compose result)
};

static void composeWorlds(std::vector<TEntry>& entries) {
  std::unordered_map<long long, size_t> byFid;
  for (size_t i = 0; i < entries.size(); i++) byFid[entries[i].fid] = i;
  // link Unity parent pointers (hierarchy teardown + finding depend on it)
  for (size_t i = 0; i < entries.size(); i++) {
    const TEntry& e = entries[i];
    if (e.father == 0 || e.t == 0) continue;
    std::unordered_map<long long, size_t>::iterator it = byFid.find(e.father);
    if (it != byFid.end()) noteParent(e.t, entries[it->second].t);
  }
  // depth memo (chain walking keeps the pass O(n))
  std::vector<int> depth(entries.size(), -1);
  for (size_t i = 0; i < entries.size(); i++) {
    std::vector<size_t> chain;
    size_t cur = i;
    while (depth[cur] < 0) {
      chain.push_back(cur);
      std::unordered_map<long long, size_t>::iterator it = byFid.find(entries[cur].father);
      if (it == byFid.end()) break;      // orphan: treated as root
      const size_t next = it->second;
      if (depth[next] >= 0) { cur = next; break; }
      cur = next;
    }
    int base = depth[cur] >= 0 ? depth[cur] : -1;
    for (size_t k = chain.size(); k-- > 0;) {
      base++;
      depth[chain[k]] = base;
    }
  }
  // depth-ordered compose: roots first, then children (Unity localTRS semantics)
  std::vector<size_t> order(entries.size());
  for (size_t i = 0; i < entries.size(); i++) order[i] = i;
  std::sort(order.begin(), order.end(),
            [&depth](size_t a, size_t b) { return depth[a] < depth[b]; });
  for (size_t oi = 0; oi < order.size(); oi++) {
    TEntry& e = entries[order[oi]];
    if (e.father != 0) {
      std::unordered_map<long long, size_t>::iterator it = byFid.find(e.father);
      if (it != byFid.end()) {
        const TEntry& p = entries[it->second];
        e.wr = p.wr * e.lr;                              // Unity: world = parent.world * local
        e.wp = p.wp + p.wr.rotate(bb::compMul(p.ws, e.lp));
        // WORLD SCALE through a ROTATED child. Unity's matrix is Rp*Sp*Rc*Sc; composing scale
        // per axis (compMul(p.ws, e.ls)) silently assumes Rc = identity. The School's big desks
        // are exactly the case where it is not: parent `Desk` scale (10,3,4) carries the
        // BoxCollider, child `CombinedMeshes_Desk` is yawed -90 and carries the mesh. Per-axis
        // composition put the parent's 10 on the mesh's own X, so after the -90 the desk ran
        // along Z -- 90 degrees off its own collider (the user's "desks face 90 degrees left or
        // right"). Each child axis now takes the parent scale ALONG THAT AXIS as rotated into
        // parent space: dot(Rc e_i, Sp (Rc e_i)). Exact for this game's 90-degree multiples (it
        // is a permutation), the diagonal of Rc^-1 Sp Rc otherwise, and IDENTICAL to the old
        // result whenever the parent scale is uniform.
        {
          const bb::Vec3 ax[3] = { e.lr.rotate(bb::Vec3(1.0f, 0.0f, 0.0f)),
                                   e.lr.rotate(bb::Vec3(0.0f, 1.0f, 0.0f)),
                                   e.lr.rotate(bb::Vec3(0.0f, 0.0f, 1.0f)) };
          const float sx = bb::dot(ax[0], bb::compMul(p.ws, ax[0]));
          const float sy = bb::dot(ax[1], bb::compMul(p.ws, ax[1]));
          const float sz = bb::dot(ax[2], bb::compMul(p.ws, ax[2]));
          e.ws = bb::compMul(bb::Vec3(sx, sy, sz), e.ls);
        }
      }
    } else {
      e.wr = e.lr;
      e.wp = e.lp;
      e.ws = e.ls;
    }
    // TEMP TRACE (desk-yaw investigation): the StudentDesk chain renders yawed ~90 deg in game;
    // this prints the REAL per-level compose values so the break can be seen, not inferred.
    static const bool trace = getenv("ENG_TRACE_COMPOSE") != 0;
    if (trace && e.go) {
      const std::string& nm = e.go->name;
      const bool want = nm.find("StudentDesk") != std::string::npos ||
                        nm == "Objects" || nm == "ScienceRoom" || nm == "Classrooms" ||
                        nm == "Rooms" || nm == "Environment";
      if (want) {
        const float lrY = 2.0f * atan2f(e.lr.y, e.lr.w) * 57.2958f;
        const float wrY = 2.0f * atan2f(e.wr.y, e.wr.w) * 57.2958f;
        printf("compose %-30s fid %-11lld father %-11lld lrYaw %8.1f wrYaw %8.1f wp(%9.2f,%8.2f,%9.2f)\n",
               nm.c_str(), e.fid, e.father, lrY, wrY, e.wp.x, e.wp.y, e.wp.z);
      }
    }
  }
  if (getenv("ENG_TRACE_COMPOSE")) {
    size_t named = 0, desks = 0, unnamed = 0;
    for (size_t i = 0; i < entries.size(); i++) {
      if (!entries[i].go) { unnamed++; continue; }
      const std::string& nm = entries[i].go->name;
      if (nm.empty()) { unnamed++; continue; }
      named++;
      if (nm.find("StudentDesk") != std::string::npos && desks < 4) {
        const TEntry& d = entries[i];
        printf("desk wpid(%9.2f,%8.2f,%9.2f) wrot(%.4f,%.4f,%.4f,%.4f) lrot(%.4f,%.4f,%.4f,%.4f) father %lld fid %lld gofid? %s\n",
               d.wp.x, d.wp.y, d.wp.z,
               d.wr.x, d.wr.y, d.wr.z, d.wr.w,
               d.lr.x, d.lr.y, d.lr.z, d.lr.w,
               d.father, d.fid, nm.c_str());
        desks++;
      }
    }
    printf("composeWorlds done: entries %d named %d unnamed %d\n",
           (int)entries.size(), (int)named, (int)unnamed);
  }
}

// ---- stock component creation + binding --------------------------------------------------------
static bb::Component* newBuiltin(const std::string& t) {
  if (t == "Camera") return new bb::Camera();
  if (t == "AudioSource") return new bb::AudioSource();
  if (t == "BoxCollider") return new bb::BoxCollider();
  if (t == "CapsuleCollider") return new bb::CapsuleCollider();
  if (t == "MeshCollider") return new bb::MeshCollider();
  if (t == "CharacterController") return new bb::CharacterController();
  if (t == "NavMeshAgent") return new bb::NavMeshAgent();
  if (t == "Rigidbody") return new bb::Rigidbody();
  if (t == "NavMeshObstacle") return new bb::NavMeshObstacle();
  if (t == "SpriteRenderer") return new bb::SpriteRenderer();
  if (t == "Animator") return new bb::Animator();
  if (t == "Canvas") return new bb::Canvas();
  if (t == "EventSystem") return new bb::EventSystem();
  return 0; // MeshFilter/CanvasRenderer/AudioListener/Behaviour/... carry no runtime state here
}

// ---- stock uGUI (UnityEngine.UI) component classes ---------------------------------------------
// These serialize as MonoBehaviours (t == "Script"), because uGUI ships as a managed assembly
// rather than as engine builtins -- so they arrive through the Script branch, not newBuiltin.
// Their class names come from the m_Script fileID (see UGUI_CLASS_BY_FILEID in tools/bake.js: one
// guid covers the whole assembly, so the fileID is what names the class).
//
// uGUI classes the game uses but the engine does not model. Skipped silently: no engine type can
// hold their state, no translated script references one, and the uGUI render/raycast pass that
// would need them is a Phase 3 item. Listing them explicitly keeps them out of the
// "script classes with no factory" warning, which must stay meaningful.
static bool isUnmodelledUgui(const std::string& name) {
  return name == "GraphicRaycaster" ||     // uGUI pointer hit-testing (Phase 3 UI input)
         name == "StandaloneInputModule" ||// uGUI input routing (engine input layer owns this)
         name == "Text" ||                 // uGUI Text; game labels are TMP -> EngineUiText
         name == "Scrollbar" || name == "ScrollRect" || name == "Mask" ||
         name == "ToggleGroup" ||          // only the excluded ControlMapper screens use these
         name == "HorizontalOrVerticalLayoutGroup" ||
         // layout/clipping inside MathGame's TMP_InputField subtree: pure uGUI render concerns,
         // no script touches them, and the UI render pass that would need them is Phase 3.
         name == "LayoutElement" || name == "RectMask2D" ||
         // iOS touch canvas: TouchController (a container: our RewiredTouchControl components
         // write the TouchControl elements directly), TouchRegion (only ProTouch, inactive) and
         // TouchInteractableTransitioner (press-feedback colours; visual only).
         name == "RewiredTouchController" || name == "RewiredTouchRegion" ||
         name == "RewiredTouchInteractableTransitioner";
}

// uGUI classes the engine DOES model. Returns 0 for anything else, so the caller falls through to
// makeScript() for the translated game classes.
static int elementTargetOf(const JsonValue* set) {   // CustomControllerElementTargetSet -> element id
  const JsonValue* tg = set ? set->find("_target") : 0;
  const JsonValue* el = tg ? tg->find("_element") : 0;
  const JsonValue* id = el ? el->find("_elementId") : 0;
  return id ? id->asInt(-1) : -1;
}

static bb::Component* newUguiScript(const std::string& cname, const JsonValue* f, BindCtx& ctx) {
  if (cname == "DpCanvasScaler") {
    bb::DpCanvasScaler* dp = new bb::DpCanvasScaler();
    if (f) {
      dp->fallbackScreenDPI = floatOf(f->find("m_FallbackScreenDPI"), 96.0f);
      dp->defaultSpriteDPI = floatOf(f->find("m_DefaultSpriteDPI"), 96.0f);
    }
    return dp;
  }
  if (cname == "RewiredTouchJoystick" || cname == "RewiredTouchPad" || cname == "RewiredTouchButton") {
    bb::RewiredTouchControl* tc = new bb::RewiredTouchControl();
    tc->kind = cname == "RewiredTouchJoystick" ? bb::RewiredTouchControl::Joystick
             : cname == "RewiredTouchPad" ? bb::RewiredTouchControl::Pad : bb::RewiredTouchControl::Button;
    if (f) {
      tc->interactable = intOf(f->find("_interactable"), 1) != 0;
      tc->visible = intOf(f->find("_visible"), 1) != 0;
      tc->stickRange = floatOf(f->find("_stickRange"), 60.0f);
      if (const JsonValue* tl = f->find("_tintTargets"))
        for (size_t i = 0; i < tl->size(); i++) {
          const JsonValue* e = tl->at(i);
          if (!e) continue;
          bb::RewiredTouchControl::TintTarget tt;
          if (const JsonValue* p = e->find("path")) tt.path = p->str;
          const char* keys[4] = { "normal", "highlighted", "pressed", "disabled" };
          bb::Color* outs[4] = { &tt.normal, &tt.highlighted, &tt.pressed, &tt.disabled };
          for (int k = 0; k < 4; k++)
            if (const JsonValue* c = e->find(keys[k]))
              if (c->size() == 4)
                *outs[k] = bb::Color(c->at(0)->asFloat(), c->at(1)->asFloat(), c->at(2)->asFloat(), c->at(3)->asFloat());
          tt.multiplier = floatOf(e->find("multiplier"), 1.0f);
          tt.fade = floatOf(e->find("fade"), 0.1f);
          tc->tints.push_back(tt);
        }
      if (const JsonValue* hl = f->find("_hideOnInvisible"))
        for (size_t i = 0; i < hl->size(); i++)
          if (hl->at(i) && hl->at(i)->type == JsonValue::String) tc->hideOnInvisible.push_back(hl->at(i)->str);
      if (tc->kind == bb::RewiredTouchControl::Button) {
        tc->elemButton = elementTargetOf(f->find("_targetCustomControllerElement"));
        tc->toggle = intOf(f->find("_buttonType"), 0) == 1;
      } else {
        const int axes = intOf(f->find("_axesToUse"), 0);   // 0 Both, 1 Horizontal, 2 Vertical
        if (axes != 2) tc->elemX = elementTargetOf(f->find("_horizontalAxisCustomControllerElement"));
        if (axes != 1) tc->elemY = elementTargetOf(f->find("_verticalAxisCustomControllerElement"));
        if (const JsonValue* st = f->find("_stickTransform")) ctx.touchSticks.push_back(std::make_pair(tc, fileIdOf(*st, "fileID")));
      }
    }
    return tc;
  }
  if (cname == "CanvasScaler") {
    bb::CanvasScaler* cs = new bb::CanvasScaler();
    if (f) {
      const JsonValue* rr = f->find("m_ReferenceResolution");
      if (rr) cs->referenceResolution = Vec2(rr->find("x") ? rr->find("x")->asFloat() : 0.0f,
                                             rr->find("y") ? rr->find("y")->asFloat() : 0.0f);
      const JsonValue* sm = f->find("m_UiScaleMode");
      if (sm) cs->uiScaleMode = sm->asInt();
      const JsonValue* mm = f->find("m_ScreenMatchMode");
      if (mm) cs->screenMatchMode = mm->asInt();
      const JsonValue* mw = f->find("m_MatchWidthOrHeight");
      if (mw) cs->matchWidthOrHeight = mw->asFloat();
      const JsonValue* sf = f->find("m_ScaleFactor");
      if (sf) cs->scaleFactor = sf->asFloat(1.0f);
    }
    return cs;
  }
  if (cname == "Slider") {
    // PlayerScript::staminaBar writes value every frame; Unity serializes the authored m_Value.
    // m_MinValue/m_MaxValue are not modelled: every call site computes an explicit 0..100 figure.
    bb::Slider* s = new bb::Slider();
    if (f) {
      s->value = floatOf(f->find("m_Value"), 0.0f);
      s->minValue = floatOf(f->find("m_MinValue"), 0.0f);
      s->maxValue = floatOf(f->find("m_MaxValue"), 1.0f);
      // m_FillRect is a component reference; resolved in the deferred pass with the other refs.
      const JsonValue* fr = f->find("m_FillRect");
      if (fr) ctx.sliderFills.push_back(std::make_pair(s, fileIdOf(*fr, "fileID")));
      const JsonValue* hr = f->find("m_HandleRect");
      if (hr) ctx.sliderHandles.push_back(std::make_pair(s, fileIdOf(*hr, "fileID")));
      s->direction = intOf(f->find("m_Direction"), 0);
      s->wholeNumbers = intOf(f->find("m_WholeNumbers"), 0) != 0;
      s->interactable = intOf(f->find("m_Interactable"), 1) != 0;
    }
    return s;
  }
  if (cname == "Button") {
    // Scripts add some listeners in code (BasicButtonScript::Start), but the MENU itself is
    // inspector-wired: m_OnClick.m_PersistentCalls carries SetActive(bool) pairs that show one
    // screen and hide another. 60 of the 63 Buttons in these scenes have them, and without them
    // every menu button is inert. Queued here, resolved after the component map is complete.
    bb::Button* btn = new bb::Button();
    if (f) {
      // Selectable transition (see eng.h Selectable). The target graphic is a component ref,
      // resolved with the other deferred refs once every component exists.
      const JsonValue* trn = f->find("m_Transition");
      if (trn) btn->transition = trn->asInt();
      const JsonValue* cols = f->find("m_Colors");
      if (cols && cols->type == JsonValue::Object) {
        const char* keys[2] = { "m_NormalColor", "m_HighlightedColor" };
        float* outs[2] = { btn->normalColor, btn->highlightedColor };
        for (int k = 0; k < 2; k++) {
          const JsonValue* c = cols->find(keys[k]);
          if (!c || c->type != JsonValue::Object) continue;
          const char* ch[4] = { "r", "g", "b", "a" };
          for (int q = 0; q < 4; q++) if (const JsonValue* v = c->find(ch[q])) outs[k][q] = v->asFloat();
        }
        btn->colorMultiplier = floatOf(cols->find("m_ColorMultiplier"), 1.0f);
      }
      const JsonValue* ss = f->find("m_SpriteState");
      const JsonValue* hs = ss ? ss->find("m_HighlightedSprite") : 0;
      const JsonValue* hg = hs ? hs->find("guid") : 0;
      if (hg && hg->type == JsonValue::String && !hg->str.empty()) btn->highlightedSprite = ctx.spriteFor(hg->str);
      // Animation transition (m_Transition 3): the triggers it fires on the button's Animator.
      // PauseMenu's Yes/No buttons use it -- Bald_Nod / Baldi_Shake controllers listen for
      // `Highlighted` / `Normal` (the in-game menu Baldi head that reacts to the pointer).
      if (const JsonValue* at = f->find("m_AnimationTriggers")) {
        const JsonValue* nt = at->find("m_NormalTrigger");
        const JsonValue* ht = at->find("m_HighlightedTrigger");
        if (nt && nt->type == JsonValue::String) btn->normalTrigger = nt->str;
        if (ht && ht->type == JsonValue::String) btn->highlightedTrigger = ht->str;
      }
      const JsonValue* tgr = f->find("m_TargetGraphic");
      if (tgr) ctx.selectableTargets.push_back(std::make_pair((bb::Selectable*)btn, fileIdOf(*tgr, "fileID")));
      const JsonValue* oc = f->find("m_OnClick");
      const JsonValue* pc = oc ? oc->find("m_PersistentCalls") : 0;
      const JsonValue* calls = pc ? pc->find("m_Calls") : 0;
      if (calls) {
        for (size_t i = 0; i < calls->size(); i++) {
          const JsonValue* call = calls->at(i);
          if (!call) continue;
          // m_CallState 0 = Off: Unity never fires those.
          const JsonValue* st = call->find("m_CallState");
          if (st && st->asInt() == 0) continue;
          const JsonValue* tgt = call->find("m_Target");
          const JsonValue* mn = call->find("m_MethodName");
          if (!tgt || !mn || mn->type != JsonValue::String) continue;
          BindCtx::PendingCall pcall;
          pcall.button = btn;
          pcall.targetFileId = fileIdOf(*tgt, "fileID");
          pcall.method = mn->str;
          pcall.boolArg = false;
          pcall.intArg = 0;
          const JsonValue* args = call->find("m_Arguments");
          const JsonValue* ba = args ? args->find("m_BoolArgument") : 0;
          if (ba) pcall.boolArg = ba->asInt() != 0;
          const JsonValue* ia = args ? args->find("m_IntArgument") : 0;
          if (ia) pcall.intArg = ia->asInt();
          const JsonValue* sa = args ? args->find("m_StringArgument") : 0;
          if (sa && sa->type == JsonValue::String) pcall.stringArg = sa->str;
          ctx.buttonCalls.push_back(pcall);
        }
      }
    }
    return btn;
  }
  if (cname == "Toggle") {
    bb::Toggle* tg = new bb::Toggle();
    if (f) tg->isOn = boolOf(f->find("m_IsOn"), false);
    return tg;
  }
  if (cname == "Image") {
    bb::Image* im = new bb::Image();
    if (f) {
      const JsonValue* sp = f->find("m_Sprite");
      if (sp && sp->type == JsonValue::Object) {
        const JsonValue* g = sp->find("guid");
        if (g && g->type == JsonValue::String && !g->str.empty()) im->sprite = ctx.spriteFor(g->str);
      }
      const JsonValue* col = f->find("m_Color");
      if (col && col->type == JsonValue::Object) {
        im->color = Color(col->find("r") ? col->find("r")->asFloat() : 1.0f,
                          col->find("g") ? col->find("g")->asFloat() : 1.0f,
                          col->find("b") ? col->find("b")->asFloat() : 1.0f,
                          col->find("a") ? col->find("a")->asFloat() : 1.0f);
      }
      im->raycastTarget = intOf(f->find("m_RaycastTarget"), 1) != 0;
    }
    return im;
  }
  if (cname == "RawImage") {
    // GC's item slots and MathGame's result marks swap .texture at runtime; the authored texture
    // is the starting state (often {fileID:0} = none).
    bb::RawImage* ri = new bb::RawImage();
    if (f) {
      const JsonValue* tx = f->find("m_Texture");
      if (tx && tx->type == JsonValue::Object) {
        const JsonValue* g = tx->find("guid");
        if (g && g->type == JsonValue::String && !g->str.empty()) ri->texture = ctx.textureFor(g->str);
      }
      const JsonValue* col = f->find("m_Color");   // Unity RawImage.color (tint)
      if (col && col->type == JsonValue::Object) {
        ri->color = Color(col->find("r") ? col->find("r")->asFloat() : 1.0f,
                          col->find("g") ? col->find("g")->asFloat() : 1.0f,
                          col->find("b") ? col->find("b")->asFloat() : 1.0f,
                          col->find("a") ? col->find("a")->asFloat() : 1.0f);
      }
      ri->raycastTarget = intOf(f->find("m_RaycastTarget"), 1) != 0;
    }
    return ri;
  }
  if (cname == "TMP_InputField") {
    // MathGame's keypad entry box (MathGameScript::playerAnswer). Unity type is TMP_InputField,
    // which derives Selectable just like the uGUI one -- the engine models both as bb::InputField.
    bb::InputField* in = new bb::InputField();
    if (f) {
      const JsonValue* tx = f->find("m_Text");
      if (tx && tx->type == JsonValue::String) in->text = tx->str;   // authored text ("" here)
      in->keyboardType = intOf(f->find("m_KeyboardType"), 0);
      const JsonValue* tc = f->find("m_TextComponent");
      if (tc) ctx.inputFieldLabels.push_back(std::make_pair(in, fileIdOf(*tc, "fileID")));
      const JsonValue* ph = f->find("m_Placeholder");
      if (ph) ctx.inputFieldPlaceholders.push_back(std::make_pair(in, fileIdOf(*ph, "fileID")));
    }
    return in;
  }
  if (cname == "EventSystem") {
    // The uGUI EventSystem is a MonoBehaviour, so it never reached newBuiltin's "EventSystem"
    // arm. state().esCurrent is set by the caller's shared registration path below.
    return new bb::EventSystem();
  }
  return 0;
}

static void bindBuiltin(bb::Component* c, const std::string& t, const JsonValue& e, BindCtx& ctx) {
  switch (t[0]) { // first-char dispatch keeps the chain short; each arm checks its exact class
    case 'A':
      if (t == "AudioSource") {
        bb::AudioSource* src = static_cast<bb::AudioSource*>(c);
        const JsonValue* clipv = e.find("clip");
        if (clipv && clipv->type == JsonValue::Object) {
          const JsonValue* g = clipv->find("guid");
          if (g && g->type == JsonValue::String && !g->str.empty()) src->clip = clipForGuid(g->str);
        }
        src->loop = boolOf(e.find("loop"), false);
        src->playOnAwake = boolOf(e.find("playOnAwake"), false);
        src->volume = floatOf(e.find("volume"), 1.0f);
        src->pitch = floatOf(e.find("pitch"), 1.0f);
        src->mute = boolOf(e.find("mute"), false);
        src->minDist = floatOf(e.find("minDist"), 1.0f);
        src->maxDist = floatOf(e.find("maxDist"), 500.0f);
        src->rolloff = intOf(e.find("rolloff"), 0);
        // "Spatial Blend" for this Unity version = panLevelCustomCurve's first key value (the
        // baker documents why; absent key = Unity default 0 = 2D).
        src->spatial = floatOf(e.find("panLevel"), 0.0f);
      } else if (t == "Animator") {
        // Only the controller reference is bound; the sprite-frame driver (Animator::Update)
        // resolves the default clip through data/anims-controllers.json on its own schedule.
        bb::Animator* an = static_cast<bb::Animator*>(c);
        an->updateMode = intOf(e.find("updateMode"), 0);
        const JsonValue* cv = e.find("controller");
        if (cv && cv->type == JsonValue::Object) {
          const JsonValue* cg = cv->find("guid");
          if (cg && cg->type == JsonValue::String) an->controllerGuid = cg->str;
        }
      }
      break;
    case 'B':
      if (t == "BoxCollider") {
        bb::BoxCollider* b = static_cast<bb::BoxCollider*>(c);
        b->isTrigger = boolOf(e.find("isTrigger"), false);
        b->size = vOf(e.find("size"));
        b->center = vOf(e.find("center"));
      }
      break;
    case 'C':
      if (t == "CapsuleCollider") {
        bb::CapsuleCollider* cap = static_cast<bb::CapsuleCollider*>(c);
        cap->isTrigger = boolOf(e.find("isTrigger"), false);
        cap->radius = floatOf(e.find("radius"), 0.5f);
        cap->height = floatOf(e.find("height"), 1.0f);
        cap->direction = intOf(e.find("direction"), 1);
        cap->center = vOf(e.find("center"));
      } else if (t == "CharacterController") {
        bb::CharacterController* cc = static_cast<bb::CharacterController*>(c);
        cc->minMoveDistance = floatOf(e.find("minMoveDistance"), 0.001f);
        cc->height = floatOf(e.find("height"), 2.0f);
        cc->radius = floatOf(e.find("radius"), 0.5f);
        cc->skinWidth = floatOf(e.find("skinWidth"), 0.08f);
        cc->center = vOf(e.find("center"));
      } else if (t == "Canvas") {
        // sortingOrder decides which overlay canvas paints over which (MathGame 1 over Hud 0).
        static_cast<bb::Canvas*>(c)->sortingOrder = intOf(e.find("sortingOrder"), 0);
      }
      break;
    case 'E':
      if (t == "EventSystem") {
        // esCurrent registration handled by the load pass (needs the component slot)
      }
      break;
    case 'M':
      if (t == "MeshCollider") {
        bb::MeshCollider* m = static_cast<bb::MeshCollider*>(c);
        m->isTrigger = boolOf(e.find("isTrigger"), false);
        m->convex = boolOf(e.find("convex"), false);   // baked by bake.js; read for Raycast ties
        // Unity collider meshes are built-in primitives 99% of the time: fileID 10210 = Quad
        // (1x1 XY panel, half-extent 0.5/0.5) and 10209 = Plane (10x10 XZ, half 5/5). Guids for
        // built-ins are the e0 resource id, so anything else is a real .asset mesh -> SurfaceNone
        // stays (raycast skips it with the one-time notice). fileID via fileIdOf: strings in
        // bakes, never through ->num.
        const JsonValue* mv = e.find("mesh");
        if (mv && mv->type == JsonValue::Object) {
          const long long id = fileIdOf(*mv, "fileID");
          if (id == 10210) {
            m->surface = bb::MeshCollider::SurfaceQuad;
            m->halfU = 0.5f; m->halfV = 0.5f;
          } else if (id == 10209) {
            m->surface = bb::MeshCollider::SurfacePlane;
            m->halfU = 5.0f; m->halfV = 5.0f;
          }
        }
      }
      break;
    case 'N':
      if (t == "NavMeshAgent") {
        bb::NavMeshAgent* a = static_cast<bb::NavMeshAgent*>(c);
        a->speed = floatOf(e.find("speed"), 0.0f);
        a->autoBraking = boolOf(e.find("autoBraking"), true);
        a->baseOffset = floatOf(e.find("baseOffset"), 0.0f);
        a->stoppingDistance = floatOf(e.find("stoppingDistance"), 0.0f);
        a->angularSpeed = floatOf(e.find("angularSpeed"), 120.0f);
        a->acceleration = floatOf(e.find("acceleration"), 8.0f);
        // radius/height/acceleration arrive in Phase 4 with the route planner
      } else if (t == "NavMeshObstacle") {
        bb::NavMeshObstacle* o = static_cast<bb::NavMeshObstacle*>(c);
        o->carving = boolOf(e.find("carving"), false);
        o->center = vOf(e.find("center"));
      }
      break;
    case 'S':
      if (t == "SpriteRenderer") {
        bb::SpriteRenderer* sr = static_cast<bb::SpriteRenderer*>(c);
        const JsonValue* sv = e.find("sprite");
        if (sv && sv->type == JsonValue::Object) {
          const JsonValue* g = sv->find("guid");
          if (g && g->type == JsonValue::String && !g->str.empty()) {
            sr->sprite = ctx.spriteFor(g->str);
          }
        }
        sr->enabled = boolOf(e.find("enabled"), true);
        const JsonValue* cv = e.find("color");
        if (cv && cv->type == JsonValue::Object) {
          sr->color = bb::Color(floatOf(cv->find("r"), 1.0f), floatOf(cv->find("g"), 1.0f),
                                floatOf(cv->find("b"), 1.0f), floatOf(cv->find("a"), 1.0f));
        }
        const JsonValue* so = e.find("sortingOrder");
        if (so) sr->sortingOrder = so->asInt();
        state().spriteRenderers.push_back(sr);
      }
      break;
    default:
      break; // Camera binds below; Rigidbody/Animator keep their serialized defaults
    case 'R':
      if (t == "Rigidbody") {   // velocity starts zeroed, as in Unity
        bb::Rigidbody* rb = static_cast<bb::Rigidbody*>(c);
        rb->isKinematic = boolOf(e.find("isKinematic"), false);
        rb->useGravity = boolOf(e.find("useGravity"), true);
        rb->drag = floatOf(e.find("drag"), 0.0f);
      }
      break;
  }
  if (t == "Camera") {
    bb::Camera* cam = static_cast<bb::Camera*>(c);
    cam->cullingMask = intOf(e.find("cullingMask"), -1);
    cam->fov = floatOf(e.find("fov"), 60.0f);
    cam->nearClip = floatOf(e.find("near"), 0.3f);
    cam->farClip = floatOf(e.find("far"), 1000.0f);
  }
}

// ---- scene load ---------------------------------------------------------------------------------
// lifelong registry membership test (scene-unload only; O(n) is fine there)
static bool goAlive(bb::GameObject* go) {
  State& g = state();
  for (size_t i = 0; i < g.gos.size(); i++) {
    if (g.gos[i] == go) return true;
  }
  return false;
}

static void destroyTree(State& g, bool keepDontDestroy) {
  // delayed destroys that outlive this scene die with it (Unity semantics)
  if (!g.doomed.empty()) {
    std::vector<DoomEntry> keep;
    for (size_t i = 0; i < g.doomed.size(); i++) {
      bb::GameObject* go = g.doomed[i].go;
      if (keepDontDestroy && go && goAlive(go) && go->dontDestroy) keep.push_back(g.doomed[i]);
      else if (goAlive(go)) destroyGameObjectNow(go); // pointer validated BEFORE any deref
    }
    g.doomed.swap(keep);
  }
  // snapshot iteration: subtree destruction compacts g.gos, so indices would go stale
  std::vector<bb::GameObject*> snapshot(g.gos.begin(), g.gos.end());
  for (size_t i = 0; i < snapshot.size(); i++) {
    bb::GameObject* go = snapshot[i];
    if (!goAlive(go)) continue;                    // already torched with an earlier subtree
    if (keepDontDestroy && go->dontDestroy) continue; // Singleton Awake-routed DDOL survives
    destroyGameObjectNow(go);
  }
  // Unity: coroutines die with their owning scene (ours hold script back-pointers)
  for (size_t i = 0; i < g.coroutines.size(); i++) delete g.coroutines[i];
  g.coroutines.clear();
}

void unloadScene(bool keepDontDestroy) {
  AudioLock audioLock;   // scene (de)construction touches audio state (State::audioMutex)
  State& g = state();
  destroyTree(g, keepDontDestroy);
  // scene-bound engine caches reset (MainCamera memo, selection, ambient restore happens in
  // scripts themselves via gc fields)
  g.camMainCache = 0;
  g.camMainCachedAtFrame = -1;
  g.esCurrent = 0;
  g.selectedGO = 0;
  g.hoveredButton = 0;          // its GameObject is about to die; next move re-hit-tests
  g.triggerOverlaps.clear();   // overlaps are scene-local; survivors re-Enter on the next step
  // Scene-local too: the nodes themselves stay alive in ownedObjects until shutdown, but
  // their gameObject/transform are about to be destroyed, so nothing may iterate them.
  g.meshRenderers.clear();
  g.spriteRenderers.clear();
  s_currentScene.clear();
  s_currentSkybox.clear();     // sky state follows the scene (see currentSceneSkybox)
  g.skybox = 0;                // ...and so does a runtime RenderSettings.skybox (Unity resets it per scene)
  g.navPolys.clear();          // the navmesh belongs to the scene
}

// Instantiate a parsed bake document (scene OR prefab) into live engine objects.
//
// Scene bakes and prefab bakes are the same format -- a .prefab carries the same GameObject /
// Transform / component documents a .unity does -- so one routine serves both: loadScene() wraps
// it for scenes, instantiatePrefab() wraps it for Instantiate().
//
// `atPos`/`atRot`, when non-null, replace the LOCAL transform of every root in the document
// (Unity's Instantiate(prefab, position, rotation) semantics). Roots have father == 0 so their
// world transform IS their local one; overriding before composeWorlds() makes the whole subtree
// compose relative to the requested placement.
//
// Returns the first root GameObject in document order, or 0 if the bake has no usable root.
static bb::GameObject* instantiateBake(const JsonValue& bake, const Vec3* atPos, const Quat* atRot) {
  State& g = state();
  BindCtx ctx; // document-local registries (goMap/comps/trs/mrs/uiText): refs resolve WITHIN the
               // document being instantiated, which is what makes prefab-internal refs work.
  ctx.comps.clear();
  ctx.trs.clear();
  ctx.mrs.clear();
  ctx.uiText.clear();
  ctx.goMap.clear();

  loadManifest();

  const JsonValue* objects = bake.find("objects");
  const JsonValue* components = bake.find("components");
  if (!objects || !components) {
    bb::Debug::Log("eng: bake lacks objects/components tables");
    return 0;
  }
  // fileID -> node indexes (O(1) during the instantiate passes)
  std::unordered_map<long long, const JsonValue*> compById;
  for (size_t i = 0; i < components->obj.size(); i++) {
    compById[atoll(components->obj[i].first.c_str())] = &components->obj[i].second;
  }
  std::unordered_map<long long, const JsonValue*> objById;
  for (size_t i = 0; i < objects->obj.size(); i++) {
    objById[atoll(objects->obj[i].first.c_str())] = &objects->obj[i].second;
  }

  std::vector<bb::Component*> startComps; // Shoot sweep source (active + enabled, load order)

  // Everything already in the registry survived the unload (DontDestroyOnLoad objects, plus the
  // Singleton-created ones). Unity runs Awake exactly ONCE per object, at creation -- so pass 4
  // must start at this watermark and never re-Awake a survivor. Re-running Awake on a survivor
  // also re-ran Start-style init against pointers into the scene that was just destroyed.
  const size_t goBase = g.gos.size();

  // ---- pass 1: GameObjects + Transforms (Unity instantiates per hierarchy/doc order) ----
  std::vector<TEntry> entries;
  for (size_t i = 0; i < objects->obj.size(); i++) {
    const long long goFid = atoll(objects->obj[i].first.c_str());
    const JsonValue& o = objects->obj[i].second;
    bb::GameObject* go = new bb::GameObject();
    go->name = o.find("n") ? o.find("n")->asString() : std::string();
    go->tag = o.find("tag") ? o.find("tag")->asString() : std::string("Untagged");
    go->layer = intOf(o.find("layer"), 0);
    go->activeSelf = boolOf(o.find("active"), true);
    noteGameObject(go);
    ctx.goMap[goFid] = go;

    const JsonValue* compsArr = o.find("comps");
    if (!compsArr) continue;
    for (size_t j = 0; j < compsArr->size(); j++) {
      const JsonValue* cv = compsArr->at(j);
      const long long cfid = cv ? cv->asFileId(0) : 0;   // exact id (string-encoded in the bake)
      if (cfid == 0) continue;
      std::unordered_map<long long, const JsonValue*>::iterator ce = compById.find(cfid);
      if (ce == compById.end()) continue;
      const std::string t = ce->second->find("t") ? ce->second->find("t")->asString() : std::string();
      if (t != "Transform" && t != "RectTransform") continue;
      TEntry entry;
      entry.t = 0;
      entry.rt = 0;
      entry.go = go;
      entry.fid = cfid;
      entry.father = fileIdOf(*ce->second, "father");   // exact id; 0 == root
      entry.lp = vOf(ce->second->find("pos"));
      entry.lr = qOf(ce->second->find("rot"));
      entry.ls = vOf(ce->second->find("scale"));
      if (t == "RectTransform") {
        // Unity: the GameObject's transform IS its RectTransform (one transform per GO). The rect
        // node joins the plain-transform machinery (registry, parent linking, world compose) and
        // also rides in go->components as a Component, exactly like Unity's identity.
        bb::RectTransform* rt = new bb::RectTransform();
        rt->go = go;
        rt->gameObject = go;
        rt->transform = rt;              // Component.transform binds to itself (Unity parity)
        go->transform = rt;
        go->components.push_back(rt);
        noteTransform(rt);
        ctx.trs[cfid] = rt;
        ctx.comps[cfid] = rt;
        entry.t = rt;                    // one compose path: composes like any Transform
        entry.rt = rt;
        const JsonValue* ap = ce->second->find("anchoredPosition");
        if (ap) {
          rt->anchoredPosition = Vec2(ap->find("x") ? ap->find("x")->asFloat() : 0.0f,
                                      ap->find("y") ? ap->find("y")->asFloat() : 0.0f);
        }
        rt->anchorMin = v2Of(ce->second->find("anchorMin"));
        rt->anchorMax = v2Of(ce->second->find("anchorMax"));
        rt->sizeDelta = v2Of(ce->second->find("sizeDelta"));
        rt->pivot = v2Of(ce->second->find("pivot"));
        if (const JsonValue* ls = ce->second->find("scale")) {
          rt->localScale = Vec2(ls->find("x") ? ls->find("x")->asFloat(1.0f) : 1.0f,
                                ls->find("y") ? ls->find("y")->asFloat(1.0f) : 1.0f);
        }
      } else {
        bb::Transform* tr = new bb::Transform();
        tr->go = go;
        go->transform = tr;
        noteTransform(tr);
        ctx.trs[cfid] = tr;
        entry.t = tr;
      }
      // Sibling index LAST: entry.t is only bound inside the two branches above. Unity paints a
      // canvas depth-first by this, which is what the uGUI draw list sorts on.
      if (entry.t) {
        const JsonValue* ro = ce->second->find("rootOrder");
        if (ro) entry.t->siblingIndex = ro->asInt();
      }
      entries.push_back(entry);
    }
  }
  // Instantiate(prefab, position, rotation): place the document's roots, then compose the subtree
  // beneath them. Scale is left at the prefab's authored value (Unity does the same).
  if (atPos || atRot) {
    for (size_t i = 0; i < entries.size(); i++) {
      if (entries[i].father != 0) continue;         // only roots are placed
      if (atPos) entries[i].lp = *atPos;
      if (atRot) entries[i].lr = *atRot;
    }
  }
  composeWorlds(entries);
  for (size_t i = 0; i < entries.size(); i++) {
    const TEntry& e = entries[i];
    if (!e.t) continue;
    e.t->position = e.wp;       // Unity Transform.position: WORLD
    e.t->localPosition = e.lp;  // Unity localPosition: the bake's authored local value (both kinds)
    e.t->rotation = e.wr;
    e.t->scale = e.ws;
  }

  // ---- pass 2: components (GO + comp order == Unity instantiation order) ----
  struct BindJob { std::string cname; bb::Component* c; const JsonValue* f; };
  std::vector<BindJob> jobs;
  bool factoryGapLogged = false;

  for (size_t i = 0; i < objects->obj.size(); i++) {
    const long long goFid = atoll(objects->obj[i].first.c_str());
    const JsonValue& o = objects->obj[i].second;
    bb::GameObject* go = ctx.goMap[goFid];
    const JsonValue* compsArr = o.find("comps");
    if (!compsArr) continue;
    for (size_t j = 0; j < compsArr->size(); j++) {
      const JsonValue* cv = compsArr->at(j);
      const long long cfid = cv ? cv->asFileId(0) : 0;   // exact id (string-encoded in the bake)
      if (cfid == 0) continue;
      std::unordered_map<long long, const JsonValue*>::iterator ce = compById.find(cfid);
      if (ce == compById.end()) continue;
      const JsonValue& e = *ce->second;
      const std::string t = e.find("t") ? e.find("t")->asString() : std::string();
      if (t == "Transform" || t == "RectTransform") continue;

      if (t == "MeshRenderer") { // Object-level (not Component) -- registry only
        bb::MeshRenderer* mr = new bb::MeshRenderer();
        mr->name = go->name;
        // Owner wiring: MeshRenderer is not in go->components, so without these the world pass
        // cannot tell which object (or transform) a renderer belongs to.
        mr->gameObject = go;
        mr->transform = go->transform;
        mr->enabled = boolOf(e.find("enabled"), true);
        // Geometry comes from the SIBLING MeshFilter: Unity keeps the mesh on the filter, not the
        // renderer. Both are built-in primitives here (10210 Quad / 10209 Plane).
        const JsonValue* goComps = o.find("comps");
        if (goComps) {
          for (size_t k = 0; k < goComps->size(); k++) {
            const JsonValue* kv = goComps->at(k);
            const long long kfid = kv ? kv->asFileId(0) : 0;
            if (kfid == 0) continue;
            std::unordered_map<long long, const JsonValue*>::iterator ke = compById.find(kfid);
            if (ke == compById.end()) continue;
            const JsonValue* kt = ke->second->find("t");
            if (!kt || kt->asString() != "MeshFilter") continue;
            const JsonValue* mv = ke->second->find("mesh");
            if (mv && mv->type == JsonValue::Object) {
              mr->meshPrimitive = (int)fileIdOf(*mv, "fileID");
              // 4300000 is shared by ALL the .asset meshes, so carry the guid too -- it is the
              // only thing that says WHICH mesh (desk vs locker vs BSODA side).
              if (mr->meshPrimitive == 4300000) {
                const JsonValue* gv = mv->find("guid");
                if (gv) mr->meshGuid = gv->asString();
              }
            }
            break;
          }
        }
        noteRuntimeObject(mr);   // nothing else owned these before: they simply leaked
        ctx.mrs[cfid] = mr;
        g.meshRenderers.push_back(mr);
        const JsonValue* mats = e.find("materials");
        if (mats && mats->size() > 0) {
          const JsonValue* m0 = mats->at(0);
          if (m0 && m0->type == JsonValue::Object) {
            const JsonValue* mg = m0->find("guid");
            if (mg && mg->type == JsonValue::String && !mg->str.empty()) {
              mr->material = ctx.materialFor(mg->str);
              if (mr->material && mr->material->mainTexGuid.empty()) {
                mr->material->mainTexGuid = materialMainTexture(mg->str);
                materialTiling(mg->str, &mr->material->texScale, &mr->material->texOffset);
                materialColor(mg->str, mr->material);
                materialAlphaTest(mg->str, mr->material);
              }
            }
          }
        }
        continue;
      }

      if (t == "Script") {
        const std::string cname = e.find("script") ? e.find("script")->asString() : std::string();
        if (cname.empty()) continue;
        const JsonValue* f = e.find("f");
        if (cname == "TextMeshProUGUI") {   // stock TMP comp -> engine UiText component
          EngineUiText* ui = new EngineUiText();
          ui->name = go->name;
          if (f) {
            const JsonValue* mt = f->find("m_text");
            if (mt && mt->type == JsonValue::String) ui->text = decodeTmpEscapes(mt->str);
            // TMP label metadata (defaults below are baked-asset values we verified; see
            // PLAN session-6). Unity's m_fontColor32 packs the Color32 struct little-endian:
            // rgba = r | (g<<8) | (b<<16) | (a<<24). Pinned against game labels with distinct
            // channels: 0xff00ff1d -> r29 g255 b0 a255, red 0xff0000ff, black 0xff000000
            // (an r-high reading would make every black label alpha-0 invisible).
            ui->fontSize = f->find("m_fontSize") ? f->find("m_fontSize")->asFloat(ui->fontSize)
                                                 : ui->fontSize;
            if (f->find("m_characterSpacing")) {
              ui->characterSpacing = f->find("m_characterSpacing")->asFloat(0.0f);
            }
            if (f->find("m_wordSpacing")) {
              ui->wordSpacing = f->find("m_wordSpacing")->asFloat(0.0f);
            }
            // m_fontColor (Color) is TMP's AUTHORITATIVE colour -- what TMP_Text.color returns and the
            // Inspector shows; m_fontColor32 is a cached copy. Of the game's 95 labels only 4 disagree,
            // and two of them are the ThinkPad's Text_Fake (0)/(1): black m_fontColor, stale WHITE
            // m_fontColor32. Reading the cache drew the two decoy questions in white over the real
            // one, so the impossible question lost its "SOLVE MATH" line and its digits came out
            // shredded. (The other two are the answer Placeholders, a 1/255 rounding difference.)
            const JsonValue* fc = f->find("m_fontColor");
            const JsonValue* c32 = f->find("m_fontColor32");
            if (fc && fc->type == JsonValue::Object && fc->find("r")) {
              ui->colorR = fc->find("r")->asFloat(1.0f);
              ui->colorG = fc->find("g") ? fc->find("g")->asFloat(1.0f) : 1.0f;
              ui->colorB = fc->find("b") ? fc->find("b")->asFloat(1.0f) : 1.0f;
              ui->colorA = fc->find("a") ? fc->find("a")->asFloat(1.0f) : 1.0f;
            } else if (c32 && c32->type == JsonValue::Object) {
              const JsonValue* rgba = c32->find("rgba");
              if (rgba && rgba->type == JsonValue::Number) {
                const unsigned int packed = (unsigned int)(rgba->num);
                ui->colorR = (float)(packed & 0xFF) / 255.0f;
                ui->colorG = (float)((packed >> 8) & 0xFF) / 255.0f;
                ui->colorB = (float)((packed >> 16) & 0xFF) / 255.0f;
                ui->colorA = (float)((packed >> 24) & 0xFF) / 255.0f;
              }
            }
            const JsonValue* al = f->find("m_textAlignment");
            if (al && al->type == JsonValue::Number) ui->alignment = al->asInt(257);
            ui->fontStyle = f->find("m_fontStyle") ? f->find("m_fontStyle")->asInt() : 0;
            ui->wordWrap = f->find("m_enableWordWrapping")
                               ? f->find("m_enableWordWrapping")->asBool(true)
                               : ui->wordWrap;
            ui->autoSize = f->find("m_enableAutoSizing")
                               ? f->find("m_enableAutoSizing")->asBool(false)
                               : false;
            // m_fontAsset -> the baked font (guid in the object body; 0/null = TMP default)
            const JsonValue* fa = f->find("m_fontAsset");
            if (fa && fa->type == JsonValue::Object) {
              const JsonValue* fg = fa->find("guid");
              if (fg && fg->type == JsonValue::String && !fg->str.empty()) {
                ui->font = fontForGuid(fg->str);
              }
            }
          }
          // Attach like any component: UiText*-typed script fields resolve through ctx.uiText,
          // but GetComponent<UiText>() needs it in go->components (DetentionTextScript::Start).
          ui->gameObject = go;
          go->components.push_back(ui);
          ui->transform = go->transform;    // Unity: the GO's transform IS its RectTransform
          ui->enabled = f ? boolOf(f->find("m_Enabled"), true) : true;
          ctx.uiText[cfid] = ui;
          ctx.comps[cfid] = ui;
          continue;
        }
        if (cname == "VerticalLayoutGroup") {
          // Layout parameters live on the group's own RectTransform; layRect reads them.
          if (bb::RectTransform* lr = dynamic_cast<bb::RectTransform*>(go->transform)) {
            lr->verticalLayout = true;
            if (f) {
              if (const JsonValue* pd = f->find("m_Padding")) {
                lr->padL = floatOf(pd->find("m_Left"), 0.0f);  lr->padR = floatOf(pd->find("m_Right"), 0.0f);
                lr->padT = floatOf(pd->find("m_Top"), 0.0f);   lr->padB = floatOf(pd->find("m_Bottom"), 0.0f);
              }
              lr->spacing = floatOf(f->find("m_Spacing"), 0.0f);
              lr->childAlignment = intOf(f->find("m_ChildAlignment"), 0);
              lr->forceExpandW = boolOf(f->find("m_ChildForceExpandWidth"), true);
              lr->forceExpandH = boolOf(f->find("m_ChildForceExpandHeight"), true);
              lr->controlW = boolOf(f->find("m_ChildControlWidth"), false);
              lr->controlH = boolOf(f->find("m_ChildControlHeight"), false);
            }
          }
          continue;
        }
        if (isTrimmedScript(cname)) continue; // charter trims (silent, matches exclusions)
        if (isUnmodelledUgui(cname)) continue; // stock uGUI with no engine behaviour (silent)
        bb::Component* c = newUguiScript(cname, f, ctx);  // stock uGUI types the engine models
        if (!c) c = makeScript(cname);                    // translated game classes
        if (!c) {
          if (!factoryGapLogged) {
            bb::Debug::Log("eng: scene carries script classes with no factory");
            factoryGapLogged = true;
          }
          continue;
        }
        c->gameObject = go;
        c->transform = go->transform;
        go->components.push_back(c);
        c->enabled = f ? boolOf(f->find("m_Enabled"), true) : true;
        ctx.comps[cfid] = c;
        // Serialized UnityEvents that live on GAME script fields (MouseoverScript.mouseOver,
        // Toggle.onValueChanged) carry the same persistent-call shape as Button.m_OnClick but are
        // resolved later against the script instance + field name. Button is skipped: its own
        // branch below already queues m_OnClick.
        if (f && cname != "Button") {
          for (size_t fi = 0; fi < f->obj.size(); fi++) {
            const JsonValue* fv = &f->obj[fi].second;
            if (!fv) continue;
            const JsonValue* pc = fv->find("m_PersistentCalls");
            if (!pc) continue;
            const JsonValue* calls = pc->find("m_Calls");
            if (!calls || calls->type != JsonValue::Array) continue;
            for (size_t i = 0; i < calls->size(); i++) {
              const JsonValue* call = calls->at(i);
              if (!call) continue;
              const JsonValue* st = call->find("m_CallState");
              if (st && st->asInt() == 0) continue;   // Off: Unity never fires those
              const JsonValue* tgt = call->find("m_Target");
              const JsonValue* mn = call->find("m_MethodName");
              if (!tgt || !mn || mn->type != JsonValue::String) continue;
              BindCtx::PendingEventCall ecall;
              ecall.owner = c;
              ecall.fieldName = f->obj[fi].first;
              ecall.targetFileId = fileIdOf(*tgt, "fileID");
              ecall.method = mn->str;
              ctx.eventCalls.push_back(ecall);
            }
          }
        }
        // The uGUI EventSystem is a MonoBehaviour, so it arrives here rather than through the
        // builtin branch's identical registration. EventSystem::current() reads this.
        if (cname == "EventSystem") g.esCurrent = static_cast<bb::EventSystem*>(c);
        jobs.push_back(BindJob{ cname, c, f });   // no-op for the stock uGUI classes
        // EVERY script is queued, active or not. Unity runs Start before the first Update of the
        // first frame the component is enabled on an active object -- for an object switched on
        // mid-game (SetActive) that is long after load. Queuing only active+enabled ones meant
        // First Prize (inactive until GameController.ActivateSpoopMode) never ran Start, so its
        // `agent = GetComponent<NavMeshAgent>()` never happened and its first Update dereferenced
        // null: the crash on the first wrong ThinkPad answer (sim: FirstPrizeScript::Update+0xb9).
        startComps.push_back(c);
        continue;
      }

      // stock engine components
      bb::Component* c = newBuiltin(t);
      if (!c) continue; // stateless comps (MeshFilter/CanvasRenderer/AudioListener/Behaviour)
      c->gameObject = go;
      c->transform = go->transform;
      go->components.push_back(c);
      c->enabled = boolOf(e.find("enabled"), true);
      ctx.comps[cfid] = c;
      bindBuiltin(c, t, e, ctx);
      if (t == "EventSystem") g.esCurrent = static_cast<bb::EventSystem*>(c);
      if (t == "CharacterController") { // Unity-consistent capsule stand-in (hit reports)
        bb::CharacterController* cc = static_cast<bb::CharacterController*>(c);
        bb::CapsuleCollider* shell = new bb::CapsuleCollider();
        shell->gameObject = go;
        shell->transform = go->transform;
        shell->radius = cc->radius;
        shell->height = cc->height;
        shell->center = cc->center;
        shell->direction = 1;
        noteComponent(shell); // raycast/trigger registry only (NOT in go->components)
        g.ccShell[cc] = shell; // owner record: the destroy sweep walks go->components, which this
                               // shell is deliberately absent from, so unnoteComponent(cc) frees it
      }
      noteComponent(c);
      // Play On Awake now lives in AudioSource::OnEnable, which the enable sweep below runs for
      // active objects and GameObject::SetActive runs when an inactive one is switched on.
    }
  }

  // ---- pass 3: script field binding (every instance exists by now) ----
  for (size_t i = 0; i < jobs.size(); i++) {
    if (!jobs[i].f) continue;   // a Script entry with no field block binds nothing
    if (s_traceLoad) printf("[load] bind %s\n", jobs[i].cname.c_str());
    bindScript(jobs[i].cname, jobs[i].c, *jobs[i].f, ctx);
  }

  // Resolve Button onClick persistent calls. SetActive(bool) on a GameObject is handled here --
  // that is the whole of menu navigation. Calls into SCRIPT methods (StartGame, ExitGame, the
  // rebind set) need the game types, which this layer deliberately cannot see; they are counted
  // and reported so the gap is visible rather than silent.
  {
    int wired = 0, unsupported = 0;
    std::set<std::string> unsupportedNames;
    for (size_t i = 0; i < ctx.buttonCalls.size(); i++) {
      const BindCtx::PendingCall& pc = ctx.buttonCalls[i];
      if (!pc.button || pc.targetFileId == 0) continue;
      if (pc.method == "SetActive") {
        bb::GameObject* target = 0;
        std::unordered_map<long long, bb::GameObject*>::iterator gi = ctx.goMap.find(pc.targetFileId);
        if (gi != ctx.goMap.end()) target = gi->second;
        if (!target) {
          // Unity wires these to a GameObject; a Transform fileID resolves through its owner.
          std::unordered_map<long long, bb::Transform*>::iterator ti = ctx.trs.find(pc.targetFileId);
          if (ti != ctx.trs.end() && ti->second) target = ti->second->go;
        }
        if (!target) continue;
        SetActiveAction* act = new SetActiveAction(target, pc.boolArg);
        pc.button->onClick.AddListener(act);
        state().ownedActions.push_back(act);
        wired++;
      } else if (pc.method == "set_text") {
        // UnityEvent calling the TMP_Text.text SETTER with the serialized string (the controls
        // page's RunInput button writes "Run" into two labels). The target is the TMP component.
        std::unordered_map<long long, bb::UiText*>::iterator ui = ctx.uiText.find(pc.targetFileId);
        if (ui == ctx.uiText.end() || !ui->second) continue;
        struct SetTextAction : public bb::UnityAction {
          bb::UiText* t; std::string v;
          SetTextAction(bb::UiText* t_, const std::string& v_) : t(t_), v(v_) {}
          void Invoke() override { if (t) t->SetText(v); }
        };
        SetTextAction* act = new SetTextAction(ui->second, pc.stringArg);
        pc.button->onClick.AddListener(act);
        state().ownedActions.push_back(act);
        wired++;
      } else {
        // A script-method call: hand it to the game-types seam. The target fileID is the COMPONENT
        // (Unity wires the script instance, not its GameObject).
        bb::Component* target = 0;
        std::unordered_map<long long, bb::Component*>::iterator ci = ctx.comps.find(pc.targetFileId);
        if (ci != ctx.comps.end()) target = ci->second;
        if (target && wireScriptCall(pc.button, target, pc.method, pc.intArg)) {
          wired++;
        } else {
          unsupported++;
          unsupportedNames.insert(pc.method);
        }
      }
    }
    if (s_traceLoad || unsupported) {
      std::string names;
      for (std::set<std::string>::iterator it = unsupportedNames.begin();
           it != unsupportedNames.end(); ++it) {
        if (!names.empty()) names += ", ";
        names += *it;
      }
      printf("[load] button onClick: %d SetActive wired, %d script calls not dispatched (%s)\n",
             wired, unsupported, names.c_str());
    }
  }

  // Resolve the game scripts' own serialized UnityEvents the same way: target components and
  // methods (Underline on TextUnderliner, UpdateState on UiSettings, ...) belong to the game
  // layer, so the resolved call goes through the same seam.
  {
    int wired = 0, unsupported = 0;
    std::set<std::string> unsupportedNames;
    for (size_t i = 0; i < ctx.eventCalls.size(); i++) {
      const BindCtx::PendingEventCall& ec = ctx.eventCalls[i];
      if (!ec.owner || ec.targetFileId == 0) continue;
      bb::Component* target = 0;
      std::unordered_map<long long, bb::Component*>::iterator ci = ctx.comps.find(ec.targetFileId);
      if (ci != ctx.comps.end()) target = ci->second;
      if (target && wirePersistentEvent(ec.owner, ec.fieldName, target, ec.method)) {
        wired++;
      } else {
        unsupported++;
        unsupportedNames.insert(ec.fieldName + "->" + ec.method);
      }
    }
    if (s_traceLoad || unsupported) {
      std::string names;
      for (std::set<std::string>::iterator it = unsupportedNames.begin();
           it != unsupportedNames.end(); ++it) {
        if (!names.empty()) names += ", ";
        names += *it;
      }
      printf("[load] script events: %d wired, %d not dispatched (%s)\n", wired, unsupported,
             names.c_str());
    }
  }

  // Resolve Slider -> fillRect now that every RectTransform exists.
  for (size_t i = 0; i < ctx.sliderFills.size(); i++) {
    const long long fid = ctx.sliderFills[i].second;
    if (fid == 0) continue;                                   // {fileID: 0} == null, Unity parity
    std::unordered_map<long long, bb::Transform*>::iterator it = ctx.trs.find(fid);
    if (it == ctx.trs.end()) continue;
    ctx.sliderFills[i].first->fillRect = dynamic_cast<bb::RectTransform*>(it->second);
  }
  for (size_t i = 0; i < ctx.sliderHandles.size(); i++) {
    const long long fid = ctx.sliderHandles[i].second;
    if (fid == 0) continue;
    std::unordered_map<long long, bb::Transform*>::iterator it = ctx.trs.find(fid);
    if (it != ctx.trs.end()) ctx.sliderHandles[i].first->handleRect = dynamic_cast<bb::RectTransform*>(it->second);
  }
  for (size_t i = 0; i < ctx.touchSticks.size(); i++) {
    const long long fid = ctx.touchSticks[i].second;
    if (fid == 0) continue;
    std::unordered_map<long long, bb::Transform*>::iterator it = ctx.trs.find(fid);
    if (it != ctx.trs.end()) ctx.touchSticks[i].first->stick = dynamic_cast<bb::RectTransform*>(it->second);
  }

  // Resolve Selectable -> m_TargetGraphic, then apply the NORMAL state at once: Unity's
  // Selectable.OnEnable does InstantClearState, so a Color Tint button is drawn in its normal
  // colour from the first frame (the controls menu's buttons are (0, 0.478, 0) green, not white).
  for (size_t i = 0; i < ctx.selectableTargets.size(); i++) {
    const long long fid = ctx.selectableTargets[i].second;
    if (fid == 0) continue;
    std::unordered_map<long long, bb::Component*>::iterator it = ctx.comps.find(fid);
    if (it == ctx.comps.end()) continue;
    ctx.selectableTargets[i].first->targetImage = dynamic_cast<bb::Image*>(it->second);
    applySelectableState(ctx.selectableTargets[i].first, false);
  }

  // Resolve TMP_InputField -> its m_TextComponent TMP label (MathGame answer box digits appear).
  State& st = state();
  for (size_t i = 0; i < ctx.inputFieldLabels.size(); i++) {
    const long long fid = ctx.inputFieldLabels[i].second;
    if (fid == 0) continue;
    std::unordered_map<long long, bb::Component*>::iterator it = ctx.comps.find(fid);
    if (it == ctx.comps.end()) continue;
    EngineUiText* label = dynamic_cast<EngineUiText*>(it->second);
    if (label) st.inputFieldLabel[ctx.inputFieldLabels[i].first] = label;
  }
  for (size_t i = 0; i < ctx.inputFieldPlaceholders.size(); i++) {
    const long long fid = ctx.inputFieldPlaceholders[i].second;
    if (fid == 0) continue;
    std::unordered_map<long long, bb::Component*>::iterator it = ctx.comps.find(fid);
    if (it == ctx.comps.end()) continue;
    EngineUiText* ph = dynamic_cast<EngineUiText*>(it->second);
    if (ph) {
      st.inputFieldPlaceholder[ctx.inputFieldPlaceholders[i].first] = ph;
      // Unity TMP_InputField::Start runs UpdateLabel: the placeholder starts visible iff the
      // field's text is empty (Unity m_Text "" authored -> visible; our authored "" too).
      ph->enabled = ctx.inputFieldPlaceholders[i].first->text.empty();
    }
  }

  // ---- pass 4: Unity load lifecycle ----
  // Awake -> OnEnable fires per ACTIVE object in instantiation order (comps in attach order),
  // for the objects THIS load created only (see goBase).
  for (size_t i = goBase; i < g.gos.size(); i++) {
    bb::GameObject* go = g.gos[i];
    if (!go->activeSelf) continue;
    for (size_t j = 0; j < go->components.size(); j++) {
      if (s_traceLoad) {
        printf("[load] awake go '%s' comp %s\n", go->name.c_str(),
               typeid(*go->components[j]).name());
      }
      go->components[j]->Awake();
      // Unity runs OnEnable only for components ACTIVE in the hierarchy; an inactive one gets it
      // when GameObject::SetActive brings it in (engine impl), not at load.
      if (go->activeInHierarchy() && go->components[j]->enabled) go->components[j]->OnEnable();
    }
  }
  // Unity Start: once per active+enabled comp, before its first Update tick -- so it is QUEUED
  // here, not called. A scene load drains the queue itself (below, in loadScene) because nothing
  // can wire a scene up between load and first frame; a prefab Instantiate deliberately leaves it
  // queued, which is the window a spawner uses to assign the clone's runtime refs before its
  // Start runs (NotebookScript sets MathGameScript::gc, whose Start immediately derefs it).
  for (size_t i = 0; i < startComps.size(); i++) {
    if (s_traceLoad) {
      printf("[load] queue start go '%s' comp %s\n",
             startComps[i]->gameObject ? startComps[i]->gameObject->name.c_str() : "<none>",
             typeid(*startComps[i]).name());
    }
    startComps[i]->startPending = true;
    g.pendingStarts.push_back(startComps[i]);
  }

  // Snapshot the freshly composed hierarchy NOW, so the very first frame's moves propagate (a pass
  // that first meets a transform takes its current pose as the baseline -- a parent moved before
  // that, by a Start or the spawner, would otherwise leave its children behind).
  propagateHierarchy();

  // First root in document order: Unity's Instantiate returns the clone's root object.
  for (size_t i = 0; i < entries.size(); i++) {
    if (entries[i].father == 0) return entries[i].go;
  }
  return g.gos.size() > goBase ? g.gos[goBase] : 0;  // no Transform at all (degenerate bake)
}

static const JsonValue* prefabBakeFor(const std::string& name);   // defined below
struct AsyncSceneLoad {
  std::string name;
  std::thread worker;
  std::atomic<bool> done{false};
  bool ok = false;
  JsonValue bake;
  std::string err;
};

// A parsed bake handed from the async worker to the next loadScene(name) (consumed once).
static JsonValue* s_preparsed = 0;
static std::string s_preparsedName;

void startAsyncSceneParse(const std::string& name) {
  State& g = state();
  if (g.asyncLoad) {                       // a second request supersedes: let the first finish
    if (g.asyncLoad->worker.joinable()) g.asyncLoad->worker.join();
    delete g.asyncLoad;
  }
  AsyncSceneLoad* a = new AsyncSceneLoad();
  a->name = name;
  g.asyncLoad = a;
  a->worker = std::thread([a]() {
    bool ok = false;
    const std::string text = jsonReadFile("data/scene-" + a->name + ".bake.json", ok);
    a->ok = ok && jsonParse(text, a->bake, a->err);
    a->done.store(true);
  });
}

void loadScene(const std::string& name) {
  AudioLock audioLock;   // scene (de)construction touches audio state (State::audioMutex)
  typedef std::chrono::steady_clock Clk;
  const Clk::time_point t0 = Clk::now();
  JsonValue bake;
  if (s_preparsed && s_preparsedName == name) {   // read + parsed by the LoadSceneAsync worker
    bake.type = s_preparsed->type;
    bake.obj.swap(s_preparsed->obj);
    bake.arr.swap(s_preparsed->arr);
    delete s_preparsed;
    s_preparsed = 0;
    s_preparsedName.clear();
  } else {
    const std::string path = "data/scene-" + name + ".bake.json";
    bool ok = false;
    const std::string text = jsonReadFile(path, ok);
    if (!ok) {
      bb::Debug::Log(("eng: missing bake " + path).c_str());
      return;
    }
    loadPump();
    std::string err;
    if (!jsonParse(text, bake, err)) {
      bb::Debug::Log(("eng: bake parse failed: " + err).c_str());
      return;
    }
  }
  const Clk::time_point tRead = Clk::now();
  loadPump();
  const Clk::time_point tParse = Clk::now();
  loadPump();
  struct Report {   // one timing line per scene load (read / parse / build), on every exit below
    const std::string& n; Clk::time_point a, b, c;
    ~Report() {
      const Clk::time_point d = Clk::now();
      printf("eng: scene '%s' loaded in %.0f ms (read %.0f, parse %.0f, build+Awake/Start %.0f)\n", n.c_str(),
             std::chrono::duration<double, std::milli>(d - a).count(), std::chrono::duration<double, std::milli>(b - a).count(),
             std::chrono::duration<double, std::milli>(c - b).count(), std::chrono::duration<double, std::milli>(d - c).count());
    }
  } report = { name, t0, tRead, tParse };
  s_currentSkybox.clear();
  const JsonValue* sk = bake.find("skybox");
  if (sk && sk->type == JsonValue::String && !sk->str.empty()) s_currentSkybox = sk->str;
  loadNavMesh(name);             // before Starts: agents may SetDestination in Start
  instantiateBake(bake, 0, 0);   // scene roots keep their authored transforms
  // Scene loads run their queued Starts immediately: there is no caller in between to wire the
  // scene up, and sim/testbed code expects a fully started scene the moment loadScene returns.
  // iOS build: the School level carries the touch UI (MobileControls -- genuine hierarchy,
  // data/prefab-MobileControls.bake.json from the iOS level2). The PC scene has none.
  if (state().mobile && name == "School")
    if (const JsonValue* mc = prefabBakeFor("MobileControls")) instantiateBake(*mc, 0, 0);
  drainPendingStarts();
  s_currentScene = name;
  // Listener cache for the 3D roll-off: establish it the moment the scene is composed. The audio
  // output thread runs at its own pace outrunning the sim frames, so play-on-awake voices can be
  // mixed before the first end-of-frame refresh happens -- with the old "no cache yet = full
  // volume" fallback that shipped one audible blip of every 3D source before the first camera
  // refresh zeroed them (observed: Buzz.wav blip at School load). Unity never has a listener-less
  // pull, and with no AudioListener at all Unity is SILENT -- the no-cache fallback below matches.
  refreshAudioSpatial();
  // --debug diagnostics: a per-scene census of the collider registries the CharacterController
  // sweep blocks against (non-trigger boxes, non-trigger capsules, Quad wall/ceiling panels).
  if (debugFlags() & kDebugDiag) {
    int trigBox = 0, trigCap = 0, trigMesh = 0;
    for (size_t i = 0; i < state().boxColliders.size(); i++)
      if (state().boxColliders[i]->isTrigger) trigBox++;
    for (size_t i = 0; i < state().capsuleColliders.size(); i++)
      if (state().capsuleColliders[i]->isTrigger) trigCap++;
    for (size_t i = 0; i < state().meshColliders.size(); i++)
      if (state().meshColliders[i]->isTrigger) trigMesh++;
    printf("CCDBG census scene=%s: box=%d (trig %d) capsule=%d (trig %d) mesh=%d (trig %d); "
           "Move blocks non-trigger boxes/capsules/Quad panels\n",
           name.c_str(), (int)state().boxColliders.size(), trigBox,
           (int)state().capsuleColliders.size(), trigCap,
           (int)state().meshColliders.size(), trigMesh);
    fflush(stdout);
  }
}

// ---- prefab instantiation (Instantiate) --------------------------------------------------------
// Prefab bakes are parsed once and cached: GC's item chain instantiates the same few prefabs
// repeatedly, and re-reading + re-parsing an 80 KB document per throw would be absurd.
// Keyed by prefab NAME (data/prefab-<Name>.bake.json).
static std::unordered_map<std::string, JsonValue>& prefabBakes() {
  static std::unordered_map<std::string, JsonValue> m;
  return m;
}

// Returns the parsed bake for `name`, or 0 when there is no such prefab bake (callers then fall
// back to the inert template-shell behaviour). A failed lookup is cached as an empty document so
// a missing prefab costs one file probe, not one per Instantiate call.
static const JsonValue* prefabBakeFor(const std::string& name) {
  std::unordered_map<std::string, JsonValue>& m = prefabBakes();
  std::unordered_map<std::string, JsonValue>::iterator it = m.find(name);
  if (it == m.end()) {
    JsonValue parsed;   // stays Null when the bake is missing or malformed
    bool ok = false;
    const std::string text = jsonReadFile("data/prefab-" + name + ".bake.json", ok);
    if (ok) {
      std::string err;
      JsonValue tmp;
      if (jsonParse(text, tmp, err)) parsed = tmp;
      else bb::Debug::Log(("eng: prefab bake parse failed: " + err).c_str());
    }
    it = m.insert(std::make_pair(name, parsed)).first;
  }
  return it->second.type == JsonValue::Object ? &it->second : 0;
}

bb::GameObject* instantiatePrefab(const std::string& name, const bb::Vec3& pos, const bb::Quat& rot) {
  const JsonValue* bake = prefabBakeFor(name);
  if (!bake) return 0;
  bb::GameObject* root = instantiateBake(*bake, &pos, &rot);
  // iOS build: the ThinkPad canvas sorts at 10 (genuine iOS sharedassets2.assets MathGame Canvas
  // m_SortingOrder 10; the PC prefab baked here has 1). That puts its full-screen black backdrop
  // over the touch controls (SimpleTouchDPI, order 1) and takes their touches, as on the device.
  if (root && state().mobile && name == "MathGame")
    if (bb::Canvas* cv = root->GetComponent<bb::Canvas>()) cv->sortingOrder = 10;
  return root;
}

void loadPump() { if (state().loadPump) state().loadPump(); }
void sceneShown() { state().audioHold.store(false); }

void processPendingScenes() {
  State& g = state();
  if (g.asyncFramesLeft > 1) g.asyncFramesLeft--;
  else if (g.asyncFramesLeft == 1 && !g.hasPendingScene) {
    // the async swap: at least asyncFramesLeft frames after the request AND the worker finished
    // (or, in deterministic runs, waited for right here)
    AsyncSceneLoad* a = g.asyncLoad;
    if (a && !a->done.load() && g.asyncWaitAtSwap) a->worker.join();
    if (!a || a->done.load()) {
      if (a) {
        if (a->worker.joinable()) a->worker.join();
        if (a->ok && a->name == g.asyncScene) {
          s_preparsed = new JsonValue();
          s_preparsed->type = a->bake.type;
          s_preparsed->obj.swap(a->bake.obj);
          s_preparsed->arr.swap(a->bake.arr);
          s_preparsedName = a->name;
        }
        delete a;
        g.asyncLoad = 0;
      }
      g.asyncFramesLeft = 0;
      g.pendingScene = g.asyncScene;   // a sync LoadScene requested meanwhile wins, as in Unity
      g.hasPendingScene = true;
    }
  }
  if (!g.hasPendingScene) return;
  g.hasPendingScene = false;
  g.asyncFramesLeft = 0;   // the swap supersedes any async request still in flight
  g.audioHold.store(true);  // until the new scene's first frame is presented (State::audioHold)
  unloadScene(true);      // DDoL branches (Singleton Awake-created) survive by contract
  loadScene(g.pendingScene);
  g.pendingScene.clear();
}

} // namespace eng
