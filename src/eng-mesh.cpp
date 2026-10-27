// eng-mesh.cpp  -  loader for the baked .asset meshes (data/mesh/<guid>.mesh).
//
// This lives in the ENGINE rather than in a backend because every backend needs the same vertex
// data: the Win32/WGL shell, the sim (which gates it), and the iOS GLKit build. The file format is
// produced by tools\bake-meshes.js, whose FORMAT comment is the contract:
//
//   'BBM1' | u32 vertCount | u32 indexCount | f32 center[3] | f32 extent[3]
//   vertCount * { f32 pos[3]; f32 uv[2] }   (20 bytes each)
//   indexCount * u16                        (triangle list)
//
// Nothing here parses a Unity asset: the blob is already flat, which is the standing rule for the
// dependency-free iOS build (same as data/tex/*.raw and data/font-*.atlas).
//
// Loads are cached by guid in State::meshAssets, INCLUDING misses (a null entry), so an
// unresolvable guid -- School has exactly one -- is probed and reported once rather than on every
// renderer, every frame.
#define _CRT_SECURE_NO_WARNINGS   // must precede every include (MSVC C4996 on fopen under /W3)
#include "eng-internal.h"

#include <cstdio>
#include <cstring>

namespace eng {

// Byte order: the blob is little-endian, and so is every target this port has (x64 testbed,
// armv7 iOS). memcpy rather than a cast keeps it alignment-safe on ARM.
static unsigned int readU32(const unsigned char* p) {
  unsigned int v = 0;
  memcpy(&v, p, 4);
  return v;
}
static float readF32(const unsigned char* p) {
  float v = 0.0f;
  memcpy(&v, p, 4);
  return v;
}

const MeshAsset* meshForGuid(const std::string& guid) {
  State& g = state();
  std::map<std::string, MeshAsset*>::iterator it = g.meshAssets.find(guid);
  if (it != g.meshAssets.end()) return it->second;
  g.meshAssets[guid] = 0;                       // cache the miss before any early return

  if (guid.empty()) return 0;
  const std::string path = "data/mesh/" + guid + ".mesh";
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    printf("eng: no mesh blob %s\n", path.c_str());
    return 0;
  }
  fseek(f, 0, SEEK_END);
  const long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<unsigned char> raw((size_t)(len > 0 ? len : 0));
  const bool read = (len > 0) && (fread(&raw[0], 1, (size_t)len, f) == (size_t)len);
  fclose(f);

  const size_t kHead = 4 + 4 + 4 + 24;
  if (!read || raw.size() < kHead) {
    printf("eng: mesh %s is too small (%d bytes)\n", guid.c_str(), (int)raw.size());
    return 0;
  }
  if (raw[0] != 'B' || raw[1] != 'B' || raw[2] != 'M' || raw[3] != '1') {
    printf("eng: mesh %s has a bad magic\n", guid.c_str());
    return 0;
  }
  const unsigned int vc = readU32(&raw[4]);
  const unsigned int ic = readU32(&raw[8]);
  const size_t vbytes = (size_t)vc * 20, ibytes = (size_t)ic * 2;
  if (vc == 0 || ic == 0 || ic % 3 != 0 || raw.size() < kHead + vbytes + ibytes) {
    printf("eng: mesh %s is malformed (%u verts, %u indices, %d bytes)\n",
           guid.c_str(), vc, ic, (int)raw.size());
    return 0;
  }

  MeshAsset* m = new MeshAsset();
  m->guid = guid;
  for (int i = 0; i < 3; i++) {
    m->center[i] = readF32(&raw[12 + i * 4]);
    m->extent[i] = readF32(&raw[24 + i * 4]);
  }
  m->verts.resize((size_t)vc * 5);
  for (size_t i = 0; i < m->verts.size(); i++) m->verts[i] = readF32(&raw[kHead + i * 4]);
  m->indices.resize(ic);
  for (size_t i = 0; i < ic; i++) {
    unsigned short v = 0;
    memcpy(&v, &raw[kHead + vbytes + i * 2], 2);
    // An out-of-range index is a GPU fault in the backend and a silent read past the end here, so
    // the whole mesh is rejected rather than clamped: a wrong triangle is worse than no desk.
    if (v >= vc) {
      printf("eng: mesh %s index %u out of range for %u verts\n", guid.c_str(), (unsigned)v, vc);
      delete m;
      return 0;
    }
    m->indices[i] = v;
  }
  g.meshAssets[guid] = m;
  return m;
}

void freeMeshAssets() {
  State& g = state();
  for (std::map<std::string, MeshAsset*>::iterator it = g.meshAssets.begin();
       it != g.meshAssets.end(); ++it) {
    delete it->second;
  }
  g.meshAssets.clear();
  for (std::map<std::string, SkyboxData*>::iterator it = g.skyboxAssets.begin();
       it != g.skyboxAssets.end(); ++it) {
    delete it->second;
  }
  g.skyboxAssets.clear();
}

// ---- RenderSettings skybox (data/skybox.json, from bake-material-textures.js) ---------------
// One flat JSON document maps every SIX-SIDED skybox material guid to its six face texture
// guids + tint + _Rotation. Nothing in the scene bakes references these textures directly (the
// reference is RenderSettings.m_SkyboxMaterial), so blobs and index entries exist only because
// bake-textures.js feeds skybox.json into its wanted set. Cached like mesh blobs, misses cached
// too (an unknown guid must not re-read the JSON every frame).
const SkyboxData* skyboxForMat(const std::string& matGuid) {
  State& g = state();
  std::map<std::string, SkyboxData*>::iterator it = g.skyboxAssets.find(matGuid);
  if (it != g.skyboxAssets.end()) return it->second;
  g.skyboxAssets[matGuid] = 0;                  // cache the miss before any early return

  bool ok = false;
  const std::string text = jsonReadFile("data/skybox.json", ok);
  if (!ok) return 0;
  JsonValue root;
  std::string err;
  if (!jsonParse(text, root, err) || root.type != JsonValue::Object) return 0;
  const JsonValue* e = root.find(matGuid.c_str());
  if (!e || e->type != JsonValue::Object) return 0;

  SkyboxData* sb = new SkyboxData();
  sb->matGuid = matGuid;
  static const char* keys[6] = { "front", "back", "left", "right", "up", "down" };
  for (int i = 0; i < 6; i++) {
    const JsonValue* f = e->find(keys[i]);
    if (f && f->type == JsonValue::String) sb->faces[i] = f->str;
  }
  // A skybox with ANY missing face is unusable as configured; Unity would render black there.
  // Keep it (the caller renders what exists) but a caller that wants a guarantee can check
  // faces[].empty() itself.
  const JsonValue* v = e->find("tintR");
  if (v && v->type == JsonValue::Number) sb->tintR = v->asFloat(sb->tintR);
  v = e->find("tintG");
  if (v && v->type == JsonValue::Number) sb->tintG = v->asFloat(sb->tintG);
  v = e->find("tintB");
  if (v && v->type == JsonValue::Number) sb->tintB = v->asFloat(sb->tintB);
  v = e->find("rotation");
  if (v && v->type == JsonValue::Number) sb->rotation = v->asFloat(sb->rotation);
  v = e->find("exposure");
  if (v && v->type == JsonValue::Number) sb->exposure = v->asFloat(sb->exposure);

  g.skyboxAssets[matGuid] = sb;
  return sb;
}

} // namespace eng
