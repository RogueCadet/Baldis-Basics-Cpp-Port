// render-scene.cpp -- the portable render passes: sky, level geometry, billboards, uGUI overlay,
// texture cache and screenshot. Lifted verbatim out of shell-win32.cpp so the iOS shell draws with
// exactly the same code; every GPU call goes through rgl (render-gl.h), and NO GL header is
// included here -- a stray GL call is a compile error, not an iOS-only breakage.
#define _CRT_SECURE_NO_WARNINGS
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "eng-internal.h"
#include "render-scene.h"


// Pixels come from the baked blobs (data/tex/<guid>.raw via data/textures-index.json) and the
// font atlas blob. The engine never decodes an image format, by design.
double g_texLoadSec = 0.0;
int g_texLoads = 0;
double qpcSecondsTex() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- real .asset meshes ----------------------------------------------------------------------
// The desks, lockers and BSODA sides are the only geometry in the game that is not a synthesised
// built-in primitive. Their vertex data is baked by tools\bake-meshes.js and loaded by the ENGINE
// (eng::meshForGuid, src/eng-mesh.cpp) rather than here, because the sim gates it and the iOS
// backend will draw from exactly the same blobs. Nothing in the C++ tree parses an asset format.

// ---- 3D world pass ---------------------------------------------------------------------------
// HANDEDNESS, stated once because it is the easiest thing to get silently wrong:
// Unity is LEFT-handed (+Z forward, +Y up). OpenGL's fixed-function helpers assume RIGHT-handed
// (-Z forward). Rather than convert every position and quaternion into GL space -- which means
// mirroring rotations and is where sign errors hide -- ALL engine data stays in Unity space and
// the handedness is absorbed by a LEFT-handed projection matrix. GL does not care: it multiplies
// whatever matrices it is given and clips against NDC. So the only non-standard object here is
// perspectiveLH(), and the view/model matrices are plain Unity TRS maths.
//
// Matrices are column-major (the layout glLoadMatrixf expects): index = column * 4 + row.

struct Mat4 { float m[16]; };

Mat4 mat4Identity() {
  Mat4 r;
  for (int i = 0; i < 16; i++) r.m[i] = 0.0f;
  r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
  return r;
}

Mat4 mat4Mul(const Mat4& a, const Mat4& b) {   // a * b
  Mat4 r;
  for (int c = 0; c < 4; c++) {
    for (int row = 0; row < 4; row++) {
      float s = 0.0f;
      for (int k = 0; k < 4; k++) s += a.m[k * 4 + row] * b.m[c * 4 + k];
      r.m[c * 4 + row] = s;
    }
  }
  return r;
}

// Standard quaternion -> rotation matrix. Unity uses this same formula for its left-handed
// quaternions, so no sign juggling is needed while we stay in Unity space.
Mat4 mat4FromQuat(const bb::Quat& q) {
  Mat4 r = mat4Identity();
  const float x = q.x, y = q.y, z = q.z, w = q.w;
  r.m[0] = 1.0f - 2.0f * (y * y + z * z);
  r.m[1] = 2.0f * (x * y + w * z);
  r.m[2] = 2.0f * (x * z - w * y);
  r.m[4] = 2.0f * (x * y - w * z);
  r.m[5] = 1.0f - 2.0f * (x * x + z * z);
  r.m[6] = 2.0f * (y * z + w * x);
  r.m[8] = 2.0f * (x * z + w * y);
  r.m[9] = 2.0f * (y * z - w * x);
  r.m[10] = 1.0f - 2.0f * (x * x + y * y);
  return r;
}

Mat4 mat4TRS(const bb::Vec3& t, const bb::Quat& q, const bb::Vec3& s) {
  Mat4 r = mat4FromQuat(q);
  for (int i = 0; i < 3; i++) { r.m[0 + i] *= s.x; r.m[4 + i] *= s.y; r.m[8 + i] *= s.z; }
  r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
  return r;
}

// Inverse of a rigid transform (rotation + translation, no scale): R^T, then -R^T * t.
Mat4 mat4ViewFrom(const bb::Vec3& pos, const bb::Quat& rot) {
  Mat4 r = mat4FromQuat(rot);
  float t[3] = { r.m[0] * pos.x + r.m[1] * pos.y + r.m[2] * pos.z,
                 r.m[4] * pos.x + r.m[5] * pos.y + r.m[6] * pos.z,
                 r.m[8] * pos.x + r.m[9] * pos.y + r.m[10] * pos.z };
  // transpose the 3x3 in place
  float tmp;
  tmp = r.m[1]; r.m[1] = r.m[4]; r.m[4] = tmp;
  tmp = r.m[2]; r.m[2] = r.m[8]; r.m[8] = tmp;
  tmp = r.m[6]; r.m[6] = r.m[9]; r.m[9] = tmp;
  r.m[12] = -t[0]; r.m[13] = -t[1]; r.m[14] = -t[2];
  return r;
}

// LEFT-handed perspective into GL's NDC (z in [-1, 1]), i.e. +Z is forward. This single matrix is
// what lets every other transform above stay in Unity space.
Mat4 perspectiveLH(float fovYDeg, float aspect, float zn, float zf) {
  Mat4 r;
  for (int i = 0; i < 16; i++) r.m[i] = 0.0f;
  const float f = 1.0f / tanf(fovYDeg * 3.14159265358979f / 360.0f);   // cot(fov/2)
  r.m[0] = f / aspect;
  r.m[5] = f;
  r.m[10] = (zf + zn) / (zf - zn);
  r.m[11] = 1.0f;                        // w = +z  (left-handed)
  r.m[14] = -2.0f * zf * zn / (zf - zn);
  return r;
}

// Determinant of a matrix's upper-left 3x3. Negative means the transform MIRRORS (an odd number
// of negative scale axes), which reverses triangle winding -- the level is full of these, e.g.
// School's ceilings at scale (-10, 10, 1).
float mat3Det(const Mat4& m) {
  return m.m[0] * (m.m[5] * m.m[10] - m.m[9] * m.m[6])
       - m.m[4] * (m.m[1] * m.m[10] - m.m[9] * m.m[2])
       + m.m[8] * (m.m[1] * m.m[6] - m.m[5] * m.m[2]);
}

// --dump-world prints one line per drawn quad on the LAST frame: object name, mesh primitive,
// world rotation, the quad's world-space normal, whether the transform mirrors, and the texture
// guid. Built because "which quad is that on screen?" was the blocker on the UV orientation bug --
// a scene has many instances of the same poster with DIFFERENT world rotations, and reasoning
// from the bake's local values picks the wrong one (that mistake is recorded in PLAN.md 7m).
bool g_dumpWorld = false;

void dumpQuad(const bb::MeshRenderer* mr, const Mat4& model, bool mirrored) {
  // world normal = model * local normal; Quad faces local +Z, Plane local +Y
  const bool isQuad = (mr->meshPrimitive == 10210);
  const float lx = 0.0f, ly = isQuad ? 0.0f : 1.0f, lz = isQuad ? 1.0f : 0.0f;
  const float nx = model.m[0] * lx + model.m[4] * ly + model.m[8] * lz;
  const float ny = model.m[1] * lx + model.m[5] * ly + model.m[9] * lz;
  const float nz = model.m[2] * lx + model.m[6] * ly + model.m[10] * lz;
  const bb::Quat& q = mr->transform->rotation;
  // The MATERIAL fields are printed too, not just the texture guid: ignoring _MainTex tiling and
  // _Color caused two separate visual bugs (walls at double brick size, the whole scene 4% too
  // bright), and both were invisible in a dump that showed only which texture was bound.
  printf("  quad '%s' prim=%d pos(%.1f,%.1f,%.1f) worldRot(%.3f,%.3f,%.3f,%.3f) "
         "normal(%.2f,%.2f,%.2f) mirrored=%d tile(%.2f,%.2f) tint(%.2f,%.2f,%.2f) tex=%s\n",
         mr->gameObject ? mr->gameObject->name.c_str() : "?", mr->meshPrimitive,
         mr->transform->position.x, mr->transform->position.y, mr->transform->position.z,
         q.x, q.y, q.z, q.w, nx, ny, nz, mirrored ? 1 : 0,
         mr->material ? mr->material->texScale.x : 1.0f,
         mr->material ? mr->material->texScale.y : 1.0f,
         mr->material ? mr->material->colorR : 1.0f,
         mr->material ? mr->material->colorG : 1.0f,
         mr->material ? mr->material->colorB : 1.0f,
         (mr->material && !mr->material->mainTexGuid.empty()) ? mr->material->mainTexGuid.c_str()
                                                              : "-");
}

// ---- billboard pass --------------------------------------------------------------------------
// Baldi, every NPC, every pickup and all the wall clutter are SpriteRenderers, not mesh geometry:
// a textured quad that faces the camera. The facing is NOT done here -- the game's own Billboard
// component (Assets\Scripts\Billboard.cs, ported 1:1) spins the transform in LateUpdate, so this
// just draws the quad the transform places. Objects WITHOUT a Billboard (the exit signs) keep
// their authored rotation, which is the behaviour the game has.
//
// Unity gives the quad no stored size: it is pixels / spritePixelsToUnits, laid out around
// spritePivot. Both come from the bake (data/sprites-index.json) and are read per sprite -- this
// game happens to use 100 PPU and a centred pivot everywhere, but assuming that would be a guess.
//
// Drawn AFTER the opaque world with blending on and depth WRITES off: sprites are cut-out art with
// a real alpha channel, and writing depth from a transparent quad punches holes in whatever is
// drawn behind it later.
// Draw order for one sprite. Unity's Sprites/Default shader is alpha-BLENDED with ZWrite off, and
// transparent geometry is ordered rather than depth-resolved: sortingOrder first, then farthest
// from the camera first. Without this, two overlapping sprites resolve by registry order, which is
// scene-file order -- stable, but arbitrary, so a nearer NPC can be painted under a farther one.
// Cutout (alpha test + ZWrite) would dodge the sort, but 6 of the 40 sprites carry anti-aliased
// edge pixels and it is not what Unity does, so parity wins over convenience here.
struct SpriteDraw {
  bb::SpriteRenderer* sr;
  int order;
  float dist2;                       // squared distance, camera to sprite origin
  bool esp;                          // -debug ESP: redraw through walls (see drawBillboards)
};

bool spriteDrawLess(const SpriteDraw& a, const SpriteDraw& b) {
  if (a.order != b.order) return a.order < b.order;   // lower sortingOrder paints first
  return a.dist2 > b.dist2;                           // then back to front
}

void drawBillboards(TexCache& tex, const Mat4& view) {
  eng::State& g = eng::state();
  if (g.spriteRenderers.empty()) return;
  bb::Camera* cam = bb::Camera::main();
  const bb::Vec3 eye = (cam && cam->transform) ? cam->transform->position : bb::Vec3(0, 0, 0);

  // -debug ESP hit test: a sprite is "a character" when its GameObject (root or ANY ancestor --
  // some NPC art hangs on a child) carries one of the names the game registered via
  // bb::debugUiEsp. Depth-test-off redraws happen later; the flag is just sorted with the quad.
  auto espHit = [&g](const bb::GameObject* go) {
    if (g.debugEspNames.empty()) return false;
    for (const bb::GameObject* o = go; o;) {
      for (size_t n = 0; n < g.debugEspNames.size(); n++)
        if (o->name == g.debugEspNames[n]) return true;
      o = (o->transform && o->transform->parent) ? o->transform->parent->go : 0;
    }
    return false;
  };

  std::vector<SpriteDraw> order;
  order.reserve(g.spriteRenderers.size());
  for (size_t i = 0; i < g.spriteRenderers.size(); i++) {
    bb::SpriteRenderer* sr = g.spriteRenderers[i];
    if (!sr->enabled || !sr->transform || !sr->sprite) continue;
    if (sr->gameObject && !sr->gameObject->activeInHierarchy()) continue;  // see drawWorld
    const bb::Vec3 d(sr->transform->position.x - eye.x,
                     sr->transform->position.y - eye.y,
                     sr->transform->position.z - eye.z);
    SpriteDraw sd;
    sd.sr = sr;
    sd.order = sr->sortingOrder;
    sd.dist2 = d.x * d.x + d.y * d.y + d.z * d.z;
    sd.esp = g.debugEspOn && espHit(sr->gameObject);
    order.push_back(sd);
  }
  if (order.empty()) return;
  std::sort(order.begin(), order.end(), spriteDrawLess);

  rgl::Enable(rgl::BLEND);
  rgl::BlendFunc(rgl::SRC_ALPHA, rgl::ONE_MINUS_SRC_ALPHA);
  rgl::Enable(rgl::ALPHA_TEST);
  rgl::AlphaFunc(rgl::GREATER, 0.01f);   // drop fully transparent texels so they never touch depth
  rgl::DepthMask(false);
  rgl::Disable(rgl::CULL_FACE);          // a billboard may be seen from its back face mid-turn

  // One billboard quad: texture lookup, ambient fold, pivot rect, flip, draw. Shared by the
  // normal depth-tested pass and (with depth testing off) the -debug ESP pass below.
  auto drawOne = [&](bb::SpriteRenderer* sr) {
    // Sprite::name IS the texture guid (BindCtx::spriteFor interns them by guid).
    std::map<std::string, SpriteGeom>::iterator it = tex.sprites.find(sr->sprite->name);
    if (it == tex.sprites.end()) return;      // not baked -> skip, never draw a placeholder
    const GLuint bind = tex.forGuid(sr->sprite->name);
    if (!bind) return;
    const SpriteGeom& sg = it->second;
    if (g_dumpWorld) {
      // --dump-world covers BILLBOARDS too. It used to print only mesh quads, which is why
      // "which sprite is that on screen?" had to be answered by matching pixel colours against
      // baked textures -- slow and inconclusive.
      printf("  sprite '%s' pos(%.1f,%.1f,%.1f) size(%.2fx%.2f units) order=%d tex=%s\n",
             sr->gameObject ? sr->gameObject->name.c_str() : "?",
             sr->transform->position.x, sr->transform->position.y, sr->transform->position.z,
             sg.unitsW, sg.unitsH, sr->sortingOrder, sr->sprite->name.c_str());
    }

    const Mat4 model = mat4TRS(sr->transform->position, sr->transform->rotation,
                               sr->transform->scale);
    const Mat4 mv = mat4Mul(view, model);
    rgl::MatrixMode(rgl::MODELVIEW);
    rgl::LoadMatrixf(mv.m);

    rgl::Enable(rgl::TEXTURE_2D);
    rgl::BindTexture(rgl::TEXTURE_2D, bind);
    // RenderSettings.ambientLight folding, same as the material pass below: white default is a
    // no-op, the finale's red ambient turns the world-space sprites with the level (alpha must
    // stay sr->color.a; tinting it would fade the sprite, not redden it).
    const bb::Color& amb = eng::state().ambientLight;
    rgl::Color4f(sr->color.r * amb.r, sr->color.g * amb.g, sr->color.b * amb.b, sr->color.a);

    // Local-space rect around the pivot, in the sprite's XY plane (Unity sprite forward is +Z).
    const float x0 = -sg.pivotX * sg.unitsW, x1 = (1.0f - sg.pivotX) * sg.unitsW;
    const float y0 = -sg.pivotY * sg.unitsH, y1 = (1.0f - sg.pivotY) * sg.unitsH;
    // v is flipped (t = 1-v) for the same reason as everywhere else: the baked blobs store row 0
    // = image top. u is NOT flipped here, unlike the built-in Quad: a sprite is drawn from its own
    // vertices rather than through Unity's Quad mesh, so it has no -Z face convention to undo.
    // Verified against ExitSign.png, which contains the word "EXIT" -- the only way to see it.
    rgl::Begin(rgl::QUADS);
    rgl::TexCoord2f(0.0f, 1.0f); rgl::Vertex3f(x0, y0, 0.0f);
    rgl::TexCoord2f(1.0f, 1.0f); rgl::Vertex3f(x1, y0, 0.0f);
    rgl::TexCoord2f(1.0f, 0.0f); rgl::Vertex3f(x1, y1, 0.0f);
    rgl::TexCoord2f(0.0f, 0.0f); rgl::Vertex3f(x0, y1, 0.0f);
    rgl::End();
  };

  for (size_t i = 0; i < order.size(); i++)
    drawOne(order[i].sr);              // visibility was filtered during the sort build

  // -debug ESP ("ESP through walls"): the marked sprites are drawn AGAIN with depth TESTING off,
  // so they read through every wall -- a plain wallhack of the same billboard quad (depth WRITES
  // were already off). The game's uGUI paints after this, so the debug menu stays on top.
  if (g.debugEspOn) {
    bool any = false;
    for (size_t i = 0; i < order.size(); i++) if (order[i].esp) { any = true; break; }
    if (any) {
      rgl::Disable(rgl::DEPTH_TEST);
      for (size_t i = 0; i < order.size(); i++)
        if (order[i].esp) drawOne(order[i].sr);
      rgl::Enable(rgl::DEPTH_TEST);
      rgl::DepthFunc(rgl::LEQUAL);     // restore what the world pass left installed
    }
  }

  rgl::DepthMask(true);
  rgl::Disable(rgl::ALPHA_TEST);
  rgl::Disable(rgl::BLEND);
}

// ---- skybox (RenderSettings.m_SkyboxMaterial) -------------------------------------------------
// Unity draws a scene's configured skybox BEFORE the geometry. School and Secret point
// RenderSettings.m_SkyboxMaterial at Material/OutsideMaterials/Skybox.mat -- the legacy SIX-SIDED
// shader (slots _FrontTex/_BackTex/_LeftTex/_RightTex/_UpTex/_DownTex, _Tint 0.502 grey,
// _Rotation 129). Without it, wherever geometry does not cover we showed our glClearColor
// (31,31,36); measured against the user's real 1.4.3 screenshot the far corridor reads
// (208,210,207) there. The other five scenes reference only Unity's built-in default material,
// which bake.js drops: currentSceneSkybox() is empty and nothing is drawn -- the Unity default
// behaviour is the same.
//
// Geometry: a camera-CENTRED cube (the shader samples by DIRECTION around the eye, so only the
// orientation of each face matters, not its distance). Corners are rotated about Y by _Rotation
// using Unity's own yaw matrix (x' = c*x + s*z; z' = -s*x + c*z, which maps Euler(0,90,0) onto
// +X). Faces, seen from INSIDE the cube (left-handed, y-up, +Z front):
//   FRONT(+Z): u 0 at x=-1 .. 1 at x=+1          RIGHT(+X): u 0 at z=+1 .. 1 at z=-1
//   BACK(-Z):  u 0 at x=+1 .. 1 at x=-1          LEFT(-X) : u 0 at z=-1 .. 1 at z=+1
//   UP(+Y):    v 1 edge toward -Z                DOWN(-Y) : v 1 edge toward +Z
// all sides v 0 at the bottom, v 1 at the top. Image TOP sits at v=1 because the baked blobs
// store row 0 = image top -- the same reason every other pass maps t = 1-v.
// Tint does NOT multiply (see the glColor block below: the real build renders the raw texels
// despite the serialized _Tint). Depth WRITES stay off and the draw is FIRST, so it cannot fight
// any geometry. The cube is emitted
// AROUND THE EYE (eye passed in): direction-only sampling means the cube must be eye-centred --
// an origin-centred cube is only accidentally visible near the spawn (a few units from the
// origin) and vanishes behind the horizon everywhere else, e.g. the playground ~245 units out.
void drawSkybox(TexCache& tex, const Mat4& view, const bb::Vec3& eye) {
  // A RUNTIME RenderSettings.skybox (eng::setSkybox) wins over the scene's. PlayerScript sets
  // BlackSky when Baldi catches you; ignoring it left the blue sky up while the game-over shrinks
  // the far clip plane (GameControllerScript: farClipPlane = gameOverDelay * 400), so the school
  // vanished into daylight -- "teleported into the skybox". An untextured skybox (BlackSky: no
  // face textures, black tint) is not a six-sided entry: nothing is drawn and drawFrame clears to
  // its colour instead.
  const bb::Material* over = eng::state().skybox;
  const std::string& matGuid = over ? over->name : eng::currentSceneSkybox();
  if (matGuid.empty()) return;
  const eng::SkyboxData* sb = eng::skyboxForMat(matGuid);
  if (!sb) return;
  if (g_dumpWorld) {
    printf("  skybox '%s' tint(%.2f,%.2f,%.2f) rotation(%.0f) faces(%.8s %.8s %.8s %.8s %.8s %.8s)\n",
           sb->matGuid.c_str(), sb->tintR, sb->tintG, sb->tintB, sb->rotation,
           sb->faces[0].c_str(), sb->faces[1].c_str(), sb->faces[2].c_str(),
           sb->faces[3].c_str(), sb->faces[4].c_str(), sb->faces[5].c_str());
  }
  rgl::Disable(rgl::BLEND);
  rgl::Disable(rgl::ALPHA_TEST);
  rgl::DepthMask(false);
  rgl::Disable(rgl::CULL_FACE);
  rgl::MatrixMode(rgl::MODELVIEW);
  // The mesh loop below glLoadMatrixf's view*model per renderer, so the modelview on entry here
  // is the PREVIOUS draw's leftover. The sky is emitted in LOCAL space around ORIGIN
  // (direction-only sampling, so the cube needs no eye offset) and must load THE VIEW itself --
  // world-space coordinates under an identity modelview was exactly the first wrong version:
  // an eye 260 units from the origin rasterised far outside every frustum, all black.
  rgl::LoadMatrixf(view.m);
  // COLOUR, from the GENUINE Skybox/6 Sided pixel program (same disassembly as the rotation):
  // out = 2 * tex * _Tint * _Exposure -- the trailing `add o0, r0, r0` is unity_ColorSpaceDouble,
  // which is 2 in Gamma colour space (ProjectSettings m_ActiveColorSpace: 0). With the authored
  // _Tint 0.502 that is 1.004: the sky shows its raw texels, as the user's real screenshot does.
  const float kr = 2.0f * sb->tintR * sb->exposure, kg = 2.0f * sb->tintG * sb->exposure,
              kb = 2.0f * sb->tintB * sb->exposure;
  rgl::Color4f(kr < 1.0f ? kr : 1.0f, kg < 1.0f ? kg : 1.0f, kb < 1.0f ? kb : 1.0f, 1.0f);
  rgl::Enable(rgl::TEXTURE_2D);

  // Unit corner offsets around the eye, half-size 1.0. Scale is free (direction-only sampling);
  // it must merely exceed the camera's near clip, which 1.0 does with room to spare.
  static const float CORNERS[8][3] = { { -1,-1,-1 }, { 1,-1,-1 }, { 1,-1,1 }, { -1,-1,1 },
                                       { -1, 1,-1 }, { 1, 1,-1 }, { 1, 1,1 }, { -1, 1,1 } };
  static const int   FIDX[6][4]  = { { 3,2,6,7 }, { 2,1,5,6 }, { 1,0,4,5 },
                                     { 0,3,7,4 }, { 7,6,5,4 }, { 0,1,2,3 } };
  // UP face was {3,2,5,4}: corners 3/2 at y=-1 and 5/4 at y=+1 -- NOT planar, a diagonal plane
  // slicing from the horizon through the zenith while sampling the UP texture at arbitrary
  // elevations (visible in the playground shot as one giant misplaced cloud blob). Correct
  // corners are the four y=+1 ones; culling is off in this pass so winding is irrelevant.
  // Draw order (+Z, +X, -Z, -X, up, down) -> faces[] (front, back, left, right, up, down).
  // Unity's 6-sided skybox puts _LeftTex on +X and _RightTex on -X (sic). MEASURED, not assumed:
  // the faces come from one panorama, so adjacent edges must agree. Mean per-channel mismatch
  // along the four horizon seams (tools/sky-seams.py): left on +X -> 2.7 / 0.0 / 4.7 / 0.5; the
  // old right-on-+X layout -> 51.0 / 0.0 / 48.1 / 0.5, i.e. clouds sliced at two seams.
  static const int   FSLOT[6]    = { 0, 2, 1, 3, 4, 5 };
  // Per-corner UV, corners ordered bl, br, tr, tl; v 0 = bottom, v 1 = top (baked blobs store row
  // 0 = image top). This must NOT be written as FUV[6][4][2] with a single-face initializer: C
  // zero-fills the other five faces, every non-front face samples texel (0,0), and the sky draws
  // as one flat colour -- exactly the "no clouds" symptom.
  // Per-corner UV, corners ordered bl, br, tr, tl. CAREFUL: two subtleties live here.
  // 1) This must NOT be written as FUV[6][4][2] with a single-face initializer: C zero-fills the
  //    other five faces, every non-front face samples texel (0,0), and the sky draws as one flat
  //    colour -- exactly the "no clouds" symptom.
  // 2) The baked blobs store row 0 = image top and the blob upload puts row 0 at t=0; every other
  //    pass compensates with t = 1-v. This pass writes glTexCoord from geometry v directly, so the
  //    table is FLIPPED (v=0 on the face-up corners) -- otherwise the sky samples upside down and
  //    the horizon band shows the image bottom: the user's real playground screenshot has
  //    sky_270's cloud chain just above the fence, which only survives with this flip.
  static const float UFC[4][2] = { { 0,1 }, { 1,1 }, { 1,0 }, { 0,0 } };
  // Rotation: x' = c*x - s*z; z' = s*x + c*z -- read from the GENUINE Skybox/6 Sided vertex
  // program (Windows build sharedassets2.assets shader 543, D3D11 bytecode disassembled with
  // d3dcompiler: sincos of _Rotation*0.017453, then dp2(( c,-s), v.xz) feeds the x column and
  // dp2((s, c), v.xz) the z column). The old sign was the inverse, "calibrated" against a
  // screenshot while the left/right faces were also swapped, so the two errors hid each other.
  const float rd = sb->rotation * 3.14159265358979f / 180.0f;
  const float c = cosf(rd), s = sinf(rd);
  for (int f = 0; f < 6; f++) {
    const std::string& g = sb->faces[FSLOT[f]];
    const GLuint bind = g.empty() ? 0 : tex.forGuid(g);
    if (!bind) continue;               // unbaked face: skip, never a placeholder colour
    rgl::BindTexture(rgl::TEXTURE_2D, bind);
    rgl::Begin(rgl::QUADS);
    for (int k = 0; k < 4; k++) {
      const int ci = FIDX[f][k];
      // LOCAL corner offset (direction-only) rotated by _Rotation about Y (genuine shader:
      // x' = c*x - s*z; z' = s*x + c*z). Every vertex
      // adds the EYE's world position because the view matrix maps world -> eye: eye-centred is
      // what makes direction-only sampling correct (an origin-centred cube only accidentally
      // worked near the spawn and vanished behind the horizon everywhere else).
      const float ox = c * CORNERS[ci][0] - s * CORNERS[ci][2];
      const float oy = CORNERS[ci][1];
      const float oz = s * CORNERS[ci][0] + c * CORNERS[ci][2];
      rgl::TexCoord2f(UFC[k][0], UFC[k][1]);
      rgl::Vertex3f(eye.x + ox, eye.y + oy, eye.z + oz);
    }
    rgl::End();
  }
  rgl::DepthMask(true);
  rgl::Enable(rgl::CULL_FACE);
}

// Draw the level. Every surface in these scenes is one of two Unity built-in primitives, so the
// geometry is two unit rectangles placed by the renderer's transform -- no mesh data is loaded.
//   10210 Quad  : 1x1 in the XY plane, normal +Z
//   10209 Plane : 10x10 in the XZ plane, normal +Y
// UVs run v=0 at the TOP row because the baked blobs store row 0 = image top, matching the font
// atlas and the uGUI path.
// PRELOAD THE SCENE'S TEXTURES. TexCache loads lazily -- a texture is read from disk and uploaded
// the first time it is DRAWN -- so every new wall, sign or billboard that came into view cost a
// mid-frame disk read + upload: measured with the --debug profiler as 130 first-use loads costing 373 ms in
// the first 100 frames of School and 38 costing 326 ms in the next 100, while walking. Unity
// loads a scene's referenced assets during the scene load, before the first frame. This does
// the same for the world renderers (MeshRenderer main textures, SpriteRenderer sprites and each
// Animator's default-clip sprite frames),
// INCLUDING inactive objects, whose assets Unity loads with the scene too. Not the whole
// data/tex folder: that is 240 MB across every scene and animation frame.
void preloadSceneTextures(TexCache& tex) {
  const double t0 = qpcSecondsTex();
  const int before = g_texLoads;
  eng::State& g = eng::state();
  for (size_t i = 0; i < g.gos.size(); i++) {
    bb::GameObject* go = g.gos[i];
    for (size_t j = 0; j < go->components.size(); j++) {
      bb::Component* c = go->components[j];
      if ((g_texLoads - before) % 16 == 15) eng::loadPump();   // cold-disk reads can take seconds
      if (bb::MeshRenderer* mr = dynamic_cast<bb::MeshRenderer*>(c)) {
        if (mr->material && !mr->material->mainTexGuid.empty()) tex.forGuid(mr->material->mainTexGuid);
      } else if (bb::SpriteRenderer* sr = dynamic_cast<bb::SpriteRenderer*>(c)) {
        if (sr->sprite && !sr->sprite->name.empty()) tex.forGuid(sr->sprite->name);
      } else if (bb::Animator* an = dynamic_cast<bb::Animator*>(c)) {
        // The DOMINANT cost, measured: Baldi's greeting wave alone was 100 lazy first-use loads
        // costing 682 ms, one ~7 ms read+upload per NEW frame while the animation played. Every
        // keyframe sprite of the default-state clip is now loaded with the scene.
        // ...and every OTHER state's frames too, now that the controller graph runs (BaldiFeed's
        // Talk/Frown, Baldi's Slap, Playtime's Sad), so the first transition does not hitch.
        std::vector<std::string> clips(1, an->clipName);
        std::map<std::string, eng::State::AnimCtrlDef>::const_iterator cd = g.animControllers.find(an->controllerGuid);
        if (cd != g.animControllers.end())
          for (std::map<std::string, eng::State::AnimStateDef>::const_iterator st = cd->second.states.begin();
               st != cd->second.states.end(); ++st) clips.push_back(st->second.clip);
        for (size_t ci = 0; ci < clips.size(); ci++) {
          std::map<std::string, eng::State::SpriteAnimKeys>::const_iterator ck = g.spriteAnims.find(clips[ci]);
          if (ck != g.spriteAnims.end())
            for (size_t k = 0; k < ck->second.keys.size(); k++)
              if (!ck->second.keys[k].second.empty()) tex.forGuid(ck->second.keys[k].second);
        }
      }
    }
  }
  const int loaded = g_texLoads - before;
  printf("shell: preloaded %d scene textures in %.0f ms (%.1f MB texture memory resident)\n", loaded,
         (qpcSecondsTex() - t0) * 1000.0, tex.gpuBytes / 1048576.0);
  // the profile's lazy-load counter should then only show what was NOT preloaded
  g_texLoads = before;
  g_texLoadSec = 0.0;
}

static double s_pass[4] = { 0, 0, 0, 0 };
static long long s_drawn = 0, s_culled = 0, s_resorts = 0;
static double passNow() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void readPassProfile(double out[4], long long& drawn, long long& culled) {
  for (int i = 0; i < 4; i++) { out[i] = s_pass[i]; s_pass[i] = 0.0; }
  drawn = s_drawn; culled = s_culled;
  if (getenv("BB_DRAWDBG")) printf("PROFILE   world re-sorts in the last 100 frames: %lld\n", s_resorts);
  s_drawn = s_culled = s_resorts = 0;
}

void drawWorld(TexCache& tex, int w, int h) {
  eng::State& g = eng::state();
  bb::Camera* cam = bb::Camera::main();
  if (!cam || !cam->transform) return;          // no main camera -> nothing to view the world from

  const float aspect = (h > 0) ? ((float)w / (float)h) : 1.0f;
  const Mat4 proj = perspectiveLH(cam->fov, aspect, cam->nearClip, cam->farClip);
  const Mat4 view = mat4ViewFrom(cam->transform->position, cam->transform->rotation);

  if (g_dumpWorld) {
    // The camera is NOT the player: CameraScript applies its own offset and look rotation,
    // so any "which side of the quad are we on" test must use THIS, not the player position.
    const bb::Vec3 cp = cam->transform->position;
    const bb::Vec3 fwd = cam->transform->forward();
    const bb::Quat cq = cam->transform->rotation;
    const bb::Vec3 up = cq.rotate(bb::Vec3(0.0f,1.0f,0.0f));
    const bb::Vec3 rt = cq.rotate(bb::Vec3(1.0f,0.0f,0.0f));
    printf("  CAMERA pos(%.2f,%.2f,%.2f) fwd(%.2f,%.2f,%.2f) up(%.2f,%.2f,%.2f) right(%.2f,%.2f,%.2f) rot(%.3f,%.3f,%.3f,%.3f)\n",
           cp.x,cp.y,cp.z, fwd.x,fwd.y,fwd.z, up.x,up.y,up.z, rt.x,rt.y,rt.z, cq.x,cq.y,cq.z,cq.w);
  }
  rgl::MatrixMode(rgl::PROJECTION);
  rgl::LoadMatrixf(proj.m);
  rgl::Enable(rgl::DEPTH_TEST);
  rgl::DepthFunc(rgl::LEQUAL);
  // Backface culling is ON because per-quad winding is corrected for mirrored transforms below
  // (see mat3Det). Without that correction the level's many negative-scale quads wind backwards
  // and culling punches holes in the walls -- which is why this used to be disabled.
  rgl::Enable(rgl::CULL_FACE);
  rgl::CullFace(rgl::BACK);
  rgl::FrontFace(rgl::CCW);

  // SKY FIRST: Unity renders the scene skybox before the geometry. The cube is emitted eye-
  // CENTRED (direction-only sampling) under the view matrix; the eye's world position decides
  // where it lands and it must follow the player, not the world origin.
  const double pt0 = passNow();
  drawSkybox(tex, view, cam->transform->position);
  const double pt1 = passNow();
  s_pass[0] += pt1 - pt0;
  if (getenv("BB_SKY_ONLY")) return;   // debug: isolate the sky pass (geometry skipped)

  // DRAW ORDER: by (cutout, texture), not scene order. Scene order switches texture on almost
  // every quad -- measured 2621 GL draw calls for one School frame on the GLES2 backend, far past
  // what SGX-era iOS 6 devices sustain. Unity does not draw in scene order either: opaque
  // geometry is sorted, and _ALPHATEST_ON materials render in the later AlphaTest queue (2450).
  // Both are depth-tested with depth writes, so order only matters for exactly coplanar overlap;
  // tools/render-compare.py measures the result against the scene-order render.
  static std::vector<bb::MeshRenderer*> sorted;
  // The sort only changes when the renderer list or a renderer's material does (Destroy /
  // Instantiate, EntranceScript's finale material swap). Re-sorting ~2600 renderers by texture-name
  // STRING every frame was a large share of an iOS-6-class CPU's frame; a pointer signature over
  // (renderer, material) is O(n) and cheap, and a change re-sorts exactly as before.
  static size_t sortedSig = 0;
  size_t sig = g.meshRenderers.size();
  for (size_t i = 0; i < g.meshRenderers.size(); i++)
    sig = sig * 1000003u ^ ((size_t)g.meshRenderers[i] + 7u * (size_t)g.meshRenderers[i]->material);
  const bool resort = sig != sortedSig || sorted.size() != g.meshRenderers.size();
  sortedSig = sig;
  if (resort) sorted.assign(g.meshRenderers.begin(), g.meshRenderers.end());
  struct ByBatch {
    static bool less(const bb::MeshRenderer* a, const bb::MeshRenderer* b) {
      const bool ca = a->material && a->material->alphaTest, cb = b->material && b->material->alphaTest;
      if (ca != cb) return !ca;
      static const std::string none;
      const std::string& ta = a->material ? a->material->mainTexGuid : none;
      const std::string& tb = b->material ? b->material->mainTexGuid : none;
      return ta < tb;
    }
  };
  if (resort) { std::stable_sort(sorted.begin(), sorted.end(), ByBatch::less); s_resorts++; }
  // FRUSTUM CULLING, as Unity culls renderers by their bounds: a renderer whose bounding sphere
  // lies wholly outside the camera's six planes is skipped. The whole School (2600+ quads) used to
  // be emitted every frame, behind the camera included -- CPU vertex work and SGX GPU work for
  // nothing on screen. View space here is left-handed, +Z forward (perspectiveLH). BB_NO_CULL=1
  // turns it off (pixel-compare against the unculled render).
  static const bool noCull = getenv("BB_NO_CULL") != 0;
  const float tanY = tanf(cam->fov * 0.5f * 0.01745329f), tanX = tanY * aspect;
  const float kX = sqrtf(1.0f + tanX * tanX), kY = sqrtf(1.0f + tanY * tanY);
  for (size_t i = 0; i < sorted.size(); i++) {
    bb::MeshRenderer* mr = sorted[i];
    if (!mr->enabled || !mr->transform) continue;
    // Three drawable kinds: the two synthesised built-in primitives, and the real .asset meshes
    // (4300000 = the CombinedMeshes_* desks/lockers/BSODA sides) whose vertex data is baked into
    // data/mesh/<guid>.mesh. A 4300000 renderer whose guid has no blob is SKIPPED, never drawn as
    // a fallback rectangle -- that put giant grey planes through the room.
    // Skip renderers using Unity BUILT-IN materials (BindCtx::materialFor names the node after
    // its guid, and built-ins are the 0000...f0 family). We have no shader or colour data for
    // those, and in these scenes every one of them is editor-only helper geometry: the
    // __AIPath_Walk_* navigation markers plus NearTrigger/ExitTrigger. They are flat quads
    // sitting at y=0, exactly COPLANAR with the floor, so drawing them z-fights a magenta/grey
    // path across the hallway that the real game never shows. Revisit if materials ever carry
    // shader + colour data.
    if (mr->material && mr->material->name.compare(0, 18, "0000000000000000f0") == 0) continue;
    const bool isQuad = (mr->meshPrimitive == 10210);
    const bool isPlane = (mr->meshPrimitive == 10209);
    const eng::MeshAsset* blob = (mr->meshPrimitive == 4300000 && !mr->meshGuid.empty())
                                   ? eng::meshForGuid(mr->meshGuid) : 0;
    if (!isQuad && !isPlane && !blob) continue;
    // activeInHierarchy, NOT activeSelf: a renderer under a disabled ancestor must not
    // draw (School's Old/PrototypeStuff subtree is exactly this case).
    if (mr->gameObject && !mr->gameObject->activeInHierarchy()) continue;

    if (!noCull) {
      // bounding sphere in local space: Quad 1x1 (half-diagonal 0.7072), Plane 10x10 (7.072), baked
      // mesh from its vertex extents; centre and radius follow the renderer's TRS (radius by the
      // largest |scale| axis, so the sphere stays conservative under non-uniform scale)
      bb::Vec3 lc(0.0f, 0.0f, 0.0f);
      float lr = isQuad ? 0.7072f : 7.072f;
      if (blob) {
        static std::unordered_map<const eng::MeshAsset*, std::pair<bb::Vec3, float> > blobBounds;
        std::unordered_map<const eng::MeshAsset*, std::pair<bb::Vec3, float> >::iterator bit = blobBounds.find(blob);
        if (bit == blobBounds.end()) {
          bb::Vec3 mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
          for (size_t v = 0; v + 4 < blob->verts.size(); v += 5) {
            mn.x = (std::min)(mn.x, blob->verts[v]); mx.x = (std::max)(mx.x, blob->verts[v]);
            mn.y = (std::min)(mn.y, blob->verts[v + 1]); mx.y = (std::max)(mx.y, blob->verts[v + 1]);
            mn.z = (std::min)(mn.z, blob->verts[v + 2]); mx.z = (std::max)(mx.z, blob->verts[v + 2]);
          }
          const bb::Vec3 c((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f);
          float r = 0.0f;
          for (size_t v = 0; v + 4 < blob->verts.size(); v += 5) {
            const float dx = blob->verts[v] - c.x, dy = blob->verts[v + 1] - c.y, dz = blob->verts[v + 2] - c.z;
            r = (std::max)(r, sqrtf(dx * dx + dy * dy + dz * dz));
          }
          bit = blobBounds.insert(std::make_pair(blob, std::make_pair(c, r))).first;
        }
        lc = bit->second.first;
        lr = bit->second.second;
      }
      const bb::Vec3& sc = mr->transform->scale;
      const float smax = (std::max)(fabsf(sc.x), (std::max)(fabsf(sc.y), fabsf(sc.z)));
      const bb::Vec3 wc = mr->transform->position +
                          mr->transform->rotation.rotate(bb::Vec3(lc.x * sc.x, lc.y * sc.y, lc.z * sc.z));
      const float r = lr * smax;
      const float vx = view.m[0] * wc.x + view.m[4] * wc.y + view.m[8] * wc.z + view.m[12];
      const float vy = view.m[1] * wc.x + view.m[5] * wc.y + view.m[9] * wc.z + view.m[13];
      const float vz = view.m[2] * wc.x + view.m[6] * wc.y + view.m[10] * wc.z + view.m[14];
      if (vz + r < cam->nearClip || vz - r > cam->farClip) { s_culled++; continue; }
      if (vx - vz * tanX > r * kX || -vx - vz * tanX > r * kX) { s_culled++; continue; }
      if (vy - vz * tanY > r * kY || -vy - vz * tanY > r * kY) { s_culled++; continue; }
    }
    s_drawn++;

    GLuint bind = 0;
    if (mr->material && !mr->material->mainTexGuid.empty()) bind = tex.forGuid(mr->material->mainTexGuid);
    // The material TINT multiplies the texel (rgl::MODULATE is the default TexEnv). These materials
    // are not white: 53 of 74 are 0.96 grey and 7 are 0.48, so drawing white made every surface
    // 4.2% too bright -- measured against a real screenshot, uniformly, which is 1/0.96 exactly.
    // RenderSettings.ambientLight -- Unity multiplies every ambient-lit (vertex-lit) shader output
    // by it. The scenes read flat because there are no lights and the ambient defaults to white,
    // so multiplying by white is the exact no-op this pass has always effectively been;
    // GameControllerScript.ExitReached() raises it to Color.red the moment the first exit is
    // sealed, and this is what turns the whole school red in the real game. Alpha untouched.
    const bb::Color& amb = eng::state().ambientLight;
    const float mcr = (mr->material ? mr->material->colorR : 1.0f) * amb.r;
    const float mcg = (mr->material ? mr->material->colorG : 1.0f) * amb.g;
    const float mcb = (mr->material ? mr->material->colorB : 1.0f) * amb.b;
    if (bind) { rgl::Enable(rgl::TEXTURE_2D); rgl::BindTexture(rgl::TEXTURE_2D, bind); rgl::Color4f(mcr, mcg, mcb, 1.0f); }
    else if (mr->material && mr->material->mainTexGuid.empty()) {
      // AUTHORED untextured (no _MainTex): Unity draws the material's _Color. This pass is unlit
      // and draws textured surfaces as texel x _Color, so an untextured one is exactly _Color.
      rgl::Disable(rgl::TEXTURE_2D); rgl::Disable(rgl::ALPHA_TEST); rgl::Color4f(mcr, mcg, mcb, 1.0f);
    }
    else      { rgl::Disable(rgl::TEXTURE_2D); rgl::Disable(rgl::ALPHA_TEST); rgl::Color4f(0.75f * mcr, 0.75f * mcg, 0.78f * mcb, 1.0f); } // texture named but not baked: placeholder

    // CUTOUT MATERIALS (Unity m_ShaderKeywords _ALPHATEST_ON: the two windows, the door
    // materials, fence -- data/material-textures.json "alphaTest"). Unity renders these as an
    // alpha-TESTED pass: fragments below _Cutoff (Standard default 0.5) discard BOTH colour and
    // depth, so a window pane's glass reveals the room behind it and the depth-tested walls keep
    // their exact behaviour. Without this the pane paints its transparent texels SOLID (their
    // stored RGB is the frame trim colours), which was the dark far-corridor patch: whatever
    // should be visible through the glass got occluded. NOT blanket blending -- blending would
    // move the 2618 opaque wall quads' depth writes, and the pane texture's alpha is BINARY
    // (0 or 255, nothing between) so blending buys nothing.
    if (mr->material && mr->material->alphaTest) {
      rgl::Enable(rgl::ALPHA_TEST);
      rgl::AlphaFunc(rgl::GREATER, 0.5f);
    } else {
      rgl::Disable(rgl::ALPHA_TEST);
    }

    const Mat4 model = mat4TRS(mr->transform->position, mr->transform->rotation, mr->transform->scale);
    const Mat4 mv = mat4Mul(view, model);
    rgl::MatrixMode(rgl::MODELVIEW);
    rgl::LoadMatrixf(mv.m);

    // Winding: a mirrored transform (negative 3x3 determinant) reverses the triangle order, so
    // emitting the same vertex sequence for every quad would make half the level back-facing.
    // Reversing the sequence for mirrored quads keeps winding consistent, which is what lets
    // backface culling be ON -- previously culling had to be disabled, which is why the Secret
    // chalkboard showed its BACK face (mirrored and inverted).
    const bool mirrored = mat3Det(model) < 0.0f;

    // UV ORIENTATION -- determined empirically, because getting it wrong is invisible on the
    // tiling wall/floor/ceiling textures and shows up only on a texture containing TEXT.
    // Test case: Secret scene ChalkWall (BaldisBlackboard.png, rotation (0,1,0,0) = 180 deg about
    // Y). Correct result is upright and readable.
    //   v: the quad's BOTTOM edge samples v=0 and its TOP edge v=1. The baked blobs store row 0 =
    //      image TOP, so this is the opposite of the naive mapping.
    //   u: increases toward the quad's local -X -- Unity's built-in Quad effectively presents its
    //      face along -Z, not +Z as the raw vertex positions suggest.
    // Both flips were verified against ChalkWall AND the School hallway, whose wall poster read
    // mirrored until the u flip.
    struct V { float x, y, z, u, v; };
    // Unity _MainTex tiling/offset: uv' = uv * scale + offset. rgl::REPEAT (set in TexCache::upload)
    // is what makes a scale > 1 actually tile instead of clamping to the edge. WhiteBrickWall is
    // (2,2), so without this every wall drew ONE stretched copy of the brick image and the bricks
    // came out double size and soft -- the visible difference against a real screenshot.
    const float tsx = mr->material ? mr->material->texScale.x : 1.0f;
    const float tsy = mr->material ? mr->material->texScale.y : 1.0f;
    const float tox = mr->material ? mr->material->texOffset.x : 0.0f;
    const float toy = mr->material ? mr->material->texOffset.y : 0.0f;
    if (blob) {
      // Baked mesh: real vertex data, so no UV guessing is involved -- but the baked TEXTURE blobs
      // store row 0 = image top while Unity's uv v=0 is the BOTTOM row, so v is flipped here. This
      // is the same single flip the primitives apply (their v=1 sits on the bottom edge); it is
      // written as 1-v rather than folded into the baker so data/mesh stays Unity-faithful.
      if (g_dumpWorld) dumpQuad(mr, model, mirrored);
      rgl::Begin(rgl::TRIANGLES);
      for (size_t t = 0; t + 2 < blob->indices.size(); t += 3) {
        for (int k = 0; k < 3; k++) {
          // WINDING IS REVERSED relative to the built-in primitives above, and that is not a
          // typo. The quads' vertex order is hand-authored to come out CCW under our
          // left-handed projection; a baked mesh carries UNITY's own index buffer, which winds
          // the other way once that projection is applied. Emitting it in file order made every
          // desk render inside-out: backface culling kept the far faces and dropped the near
          // ones, so you looked through the desktop into the hollow interior. Mirrored
          // transforms then flip it back, exactly as for the quads.
          const unsigned short vi = blob->indices[t + (mirrored ? k : (2 - k))];
          const float* vp = &blob->verts[(size_t)vi * 5];
          rgl::TexCoord2f(vp[3] * tsx + tox, (1.0f - vp[4]) * tsy + toy);
          rgl::Vertex3f(vp[0], vp[1], vp[2]);
        }
      }
      rgl::End();
      continue;
    }
    V q[4];
    if (isQuad) {                                // Quad: 1x1 in the XY plane, normal +Z
      const V a[4] = { { -0.5f, -0.5f, 0.0f, 0.0f, 1.0f },
                       {  0.5f, -0.5f, 0.0f, 1.0f, 1.0f },
                       {  0.5f,  0.5f, 0.0f, 1.0f, 0.0f },
                       { -0.5f,  0.5f, 0.0f, 0.0f, 0.0f } };
      for (int k = 0; k < 4; k++) q[k] = a[k];
    } else {                                     // Plane: 10x10 in the XZ plane, normal +Y
      const V a[4] = { { -5.0f, 0.0f, -5.0f, 0.0f, 1.0f },
                       {  5.0f, 0.0f, -5.0f, 1.0f, 1.0f },
                       {  5.0f, 0.0f,  5.0f, 1.0f, 0.0f },
                       { -5.0f, 0.0f,  5.0f, 0.0f, 0.0f } };
      for (int k = 0; k < 4; k++) q[k] = a[k];
    }
    if (g_dumpWorld) dumpQuad(mr, model, mirrored);
    rgl::Begin(rgl::QUADS);
    for (int k = 0; k < 4; k++) {
      const V& vtx = q[mirrored ? (3 - k) : k];
      rgl::TexCoord2f(vtx.u * tsx + tox, vtx.v * tsy + toy);
      rgl::Vertex3f(vtx.x, vtx.y, vtx.z);
    }
    rgl::End();
  }
  const double pt2 = passNow();
  s_pass[1] += pt2 - pt1;
  drawBillboards(tex, view);
  s_pass[2] += passNow() - pt2;

  // Hand the UI pass a clean state: it is screen-space, single-winding, and must not be
  // depth-tested or culled. Leaving rgl::CULL_FACE on here silently ate the whole HUD.
  rgl::Disable(rgl::DEPTH_TEST);
  rgl::Disable(rgl::CULL_FACE);
}

void drawFrame(TexCache& tex, int w, int h) {
  eng::State& g = eng::state();
  rgl::Viewport(0, 0, w, h);
  {
    float cr = 0.12f, cg = 0.12f, cb = 0.14f;
    const bb::Material* over = g.skybox;               // see drawSkybox: an untextured runtime sky
    if (over && !eng::skyboxForMat(over->name)) { cr = over->colorR; cg = over->colorG; cb = over->colorB; }
    rgl::ClearColor(cr, cg, cb, 1.0f);
  }
  rgl::Clear(rgl::COLOR_BUFFER_BIT | rgl::DEPTH_BUFFER_BIT);

  drawWorld(tex, w, h);        // 3D level first; the uGUI pass below is a screen-space overlay
  const double ptUi = passNow();
  struct UiPassTimer { double t; ~UiPassTimer() { s_pass[3] += passNow() - t; } } uiPassTimer = { ptUi };

  // Row space: origin top-left, y DOWN -- exactly the convention DrawQuad uses, so no flip here.
  rgl::MatrixMode(rgl::PROJECTION);
  rgl::LoadIdentity();
  rgl::Ortho(0.0, (double)g.screenW, (double)g.screenH, 0.0, -1.0, 1.0);
  rgl::MatrixMode(rgl::MODELVIEW);
  rgl::LoadIdentity();
  rgl::Enable(rgl::BLEND);
  rgl::BlendFunc(rgl::SRC_ALPHA, rgl::ONE_MINUS_SRC_ALPHA);

  for (size_t i = 0; i < g.drawList.size(); i++) {
    const eng::DrawQuad& q = g.drawList[i];
    GLuint bind = 0;
    if (q.textured) {
      if (q.texId < 0) bind = tex.forFont(q.font);   // texId < 0: that quad's font atlas
      else if ((size_t)q.texId < g.drawTextures.size()) bind = tex.forGuid(g.drawTextures[q.texId]);
    }
    if (bind) { rgl::Enable(rgl::TEXTURE_2D); rgl::BindTexture(rgl::TEXTURE_2D, bind); }
    else      { rgl::Disable(rgl::TEXTURE_2D); }   // solid tint fill (unbaked / sprite-less)
    // SDF glyphs: alpha-TEST the distance at 0.5 and draw what passes opaque (blend off); bitmap
    // glyphs and images keep ordinary alpha blending.
    const bool sdfGlyph = q.textured && q.texId < 0 && q.font && q.font->sdf;
    if (sdfGlyph) { rgl::Disable(rgl::BLEND); rgl::Enable(rgl::ALPHA_TEST); rgl::AlphaFunc(rgl::GEQUAL, 0.5f); }
    rgl::Color4f(q.r, q.g, q.b, q.a);
    rgl::Begin(rgl::QUADS);
      rgl::TexCoord2f(q.u0, q.v0); rgl::Vertex2f(q.x0, q.y0);
      rgl::TexCoord2f(q.u1, q.v0); rgl::Vertex2f(q.x1, q.y0);
      rgl::TexCoord2f(q.u1, q.v1); rgl::Vertex2f(q.x1, q.y1);
      rgl::TexCoord2f(q.u0, q.v1); rgl::Vertex2f(q.x0, q.y1);
    rgl::End();
    if (sdfGlyph) { rgl::Disable(rgl::ALPHA_TEST); rgl::Enable(rgl::BLEND); }
  }
}

// glReadPixels the framebuffer to raw RGBA8 plus a sidecar naming the dimensions. Verifying the
// GL path otherwise needs a human looking at a window; tools/shot-to-png.js turns it into a PNG.
void screenshot(const char* path, int w, int h) {
  std::vector<unsigned char> px((size_t)w * h * 4);
  rgl::ReadPixels(w, h, &px[0]);
  // GL origin is bottom-left; flip to top-left rows so the blob matches every other image here.
  std::vector<unsigned char> flip((size_t)w * h * 4);
  for (int y = 0; y < h; y++) {
    memcpy(&flip[(size_t)y * w * 4], &px[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
  }
  FILE* f = fopen(path, "wb");
  if (!f) { printf("shell: cannot write %s\n", path); return; }
  fwrite(&flip[0], 1, flip.size(), f);
  fclose(f);
  char meta[512];
  snprintf(meta, sizeof(meta), "%s.txt", path);
  FILE* m = fopen(meta, "wb");
  if (m) { fprintf(m, "%d %d\n", w, h); fclose(m); }
  printf("shell: wrote %s (%dx%d raw RGBA8)\n", path, w, h);
}


