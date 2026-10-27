// render-scene.h -- the portable render passes shared by the Windows and iOS shells.
// Implementation: render-scene.cpp (GPU access only through rgl, render-gl.h).
#pragma once
#include <map>
#include <string>
#include <vector>
#include <cstdio>

#include "eng-internal.h"
#include "render-gl.h"

typedef rgl::Tex GLuint;

// ---- GPU texture cache ---------------------------------------------------------------------
// Sprite geometry (data/sprites-index.json). A SpriteRenderer stores no size: Unity derives the
// quad from pixels / spritePixelsToUnits around spritePivot, so those come from the bake.
struct SpriteGeom {
  float unitsW, unitsH, pivotX, pivotY;
  SpriteGeom() : unitsW(0), unitsH(0), pivotX(0.5f), pivotY(0.5f) {}
};

// --debug profiler: cost of LAZY texture loads (disk read + GL upload on a texture's first draw).
extern double g_texLoadSec;   // --debug profiler (defined in render-scene.cpp)
extern int g_texLoads;
double qpcSecondsTex();

struct TexCache {
  std::map<std::string, GLuint> byGuid;
  // w/h plus the Unity import settings the upload must honour (point filtering, mipmaps).
  struct TexInfo { int w, h; bool point, mip; TexInfo() : w(0), h(0), point(true), mip(false) {} };
  std::map<std::string, TexInfo> dims;
  std::map<std::string, SpriteGeom> sprites;
  GLuint atlas;
  size_t gpuBytes;          // texture memory uploaded so far (mip 0, RGBA8)
  TexCache() : atlas(0), gpuBytes(0) {}

  // GLES 2.0 texture rules (POT repeat / NPOT clamp, mips only on POT) live in rgl::upload.
  static GLuint upload(const unsigned char* px, int w, int h, bool point, bool mip) {
    return rgl::upload(px, w, h, point, mip);
  }

  static bool readFile(const std::string& path, std::vector<unsigned char>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    const bool ok = (n <= 0) || (fread(&out[0], 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    return ok;
  }

  void loadIndex() {
    bool ok = false;
    const std::string text = eng::jsonReadFile("data/textures-index.json", ok);
    if (!ok) { printf("shell: no data/textures-index.json (run tools/bake-textures.js)\n"); return; }
    eng::JsonValue root;
    std::string err;
    if (!eng::jsonParse(text, root, err)) { printf("shell: index parse: %s\n", err.c_str()); return; }
    for (size_t i = 0; i < root.obj.size(); i++) {
      const eng::JsonValue& v = root.obj[i].second;
      const eng::JsonValue* w = v.find("w");
      const eng::JsonValue* h = v.find("h");
      if (!w || !h) continue;
      TexInfo ti;
      ti.w = w->asInt();
      ti.h = h->asInt();
      const eng::JsonValue* pt = v.find("point");
      const eng::JsonValue* mp = v.find("mip");
      if (pt) ti.point = pt->asInt() != 0;        // absent -> point, the overwhelming majority
      if (mp) ti.mip = mp->asInt() != 0;
      dims[root.obj[i].first] = ti;
    }
    printf("shell: textures-index has %d entries\n", (int)dims.size());
  }

  void loadSpriteIndex() {
    bool ok = false;
    const std::string text = eng::jsonReadFile("data/sprites-index.json", ok);
    if (!ok) { printf("shell: no data/sprites-index.json (run tools/bake-textures.js)\n"); return; }
    eng::JsonValue root;
    std::string err;
    if (!eng::jsonParse(text, root, err)) { printf("shell: sprite index parse: %s\n", err.c_str()); return; }
    for (size_t i = 0; i < root.obj.size(); i++) {
      const eng::JsonValue& v = root.obj[i].second;
      const eng::JsonValue* uw = v.find("unitsW");
      const eng::JsonValue* uh = v.find("unitsH");
      if (!uw || !uh) continue;
      SpriteGeom g;
      g.unitsW = uw->asFloat(0.0f);
      g.unitsH = uh->asFloat(0.0f);
      const eng::JsonValue* px = v.find("pivotX");
      const eng::JsonValue* py = v.find("pivotY");
      if (px) g.pivotX = px->asFloat(0.5f);
      if (py) g.pivotY = py->asFloat(0.5f);
      sprites[root.obj[i].first] = g;
    }
    printf("shell: sprites-index has %d entries\n", (int)sprites.size());
  }

  GLuint forGuid(const std::string& guid) {
    std::map<std::string, GLuint>::iterator it = byGuid.find(guid);
    if (it != byGuid.end()) return it->second;
    const double tLoad0 = qpcSecondsTex();
    GLuint id = 0;
    std::map<std::string, TexInfo>::iterator d = dims.find(guid);
    std::vector<unsigned char> px;
    if (d != dims.end() && readFile("data/tex/" + guid + ".raw", px) &&
        px.size() == (size_t)(d->second.w * d->second.h * 4)) {
      id = upload(&px[0], d->second.w, d->second.h, d->second.point, d->second.mip);
      gpuBytes += px.size();   // RGBA8 resident on the GPU (memory budget for 256-512 MB iOS 6 devices)
    }
    byGuid[guid] = id;        // cache misses too, so a bad guid is probed once
    g_texLoadSec += qpcSecondsTex() - tLoad0;   // --debug profiler: lazy first-use load cost
    g_texLoads++;
    return id;
  }

  // Per-font atlas textures (DrawQuad::font). Bitmap faces (COMIC, COMIC_Pro) are pixel art:
  // point-sampled. SDF faces need LINEAR sampling for the alpha test to find a smooth 0.5 edge.
  std::map<const eng::TMPFontData*, GLuint> fontAtlases;
  GLuint forFont(const eng::TMPFontData* f) {
    if (!f) return atlas;
    std::map<const eng::TMPFontData*, GLuint>::iterator it = fontAtlases.find(f);
    if (it != fontAtlases.end()) return it->second;
    GLuint id = 0;
    if (!f->rgba.empty() && (int)f->rgba.size() == f->atlasW * f->atlasH * 4)
      id = upload((const unsigned char*)&f->rgba[0], f->atlasW, f->atlasH, !f->sdf, false);
    fontAtlases[f] = id;
    return id;
  }

  // The original single atlas (COMIC_Pro), kept as the fallback for quads without a font.
  void loadAtlas() {
    std::vector<unsigned char> px;
    if (!readFile("data/font-COMIC_Pro.atlas", px)) { printf("shell: no COMIC_Pro atlas\n"); return; }
    const int w = 128, h = 256;
    if (px.size() != (size_t)(w * h * 4)) { printf("shell: atlas size %d\n", (int)px.size()); return; }
    atlas = upload(&px[0], w, h, true, false);   // the baked glyph page is pixel art: point, no mips
  }
};


extern bool g_dumpWorld;                  // --dump-world: print every drawn quad on the frame
void preloadSceneTextures(TexCache& tex);
// Render-pass profile (shell --debug / app-core perf log): seconds per pass since the last read,
// [0] sky [1] world loop [2] billboards [3] uGUI emit; counters: renderers drawn / culled.
void readPassProfile(double out[4], long long& drawn, long long& culled);  // the scene's referenced textures, before frame 1
void drawFrame(TexCache& tex, int w, int h);
void screenshot(const char* path, int w, int h);
