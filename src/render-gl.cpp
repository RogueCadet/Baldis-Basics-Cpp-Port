// render-gl.cpp -- the two rgl backends (see render-gl.h for why the layer exists).
// RGL_FORCE_ES2 compiles the iOS branch anywhere: tools/build-sim32.ps1 checks it against the
// Khronos GLES2 headers (Apple's <OpenGLES/ES2/gl.h> follows that API) on Windows.
#if defined(__APPLE__) || defined(RGL_FORCE_ES2)
#include <OpenGLES/ES2/gl.h>
#define RGL_ES2_ONLY 1          // iOS: there is no fixed-function GL to fall back to
#else
#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#endif
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

#include "render-gl.h"

// ---- desktop GL 2.0 entry points (Windows ships GL 1.1 headers; 2.0 comes via wglGetProcAddress)
#if !defined(RGL_ES2_ONLY)
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_GENERATE_MIPMAP 0x8191
#define RGL_FN(ret, name, args) typedef ret (APIENTRY* PFN_##name) args; static PFN_##name name##_ = 0;
RGL_FN(GLuint, glCreateShader, (GLenum))
RGL_FN(void, glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))
RGL_FN(void, glCompileShader, (GLuint))
RGL_FN(void, glGetShaderiv, (GLuint, GLenum, GLint*))
RGL_FN(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))
RGL_FN(GLuint, glCreateProgram, (void))
RGL_FN(void, glAttachShader, (GLuint, GLuint))
RGL_FN(void, glBindAttribLocation, (GLuint, GLuint, const GLchar*))
RGL_FN(void, glLinkProgram, (GLuint))
RGL_FN(void, glGetProgramiv, (GLuint, GLenum, GLint*))
RGL_FN(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))
RGL_FN(void, glUseProgram, (GLuint))
RGL_FN(GLint, glGetUniformLocation, (GLuint, const GLchar*))
RGL_FN(void, glUniform1i, (GLint, GLint))
RGL_FN(void, glUniform1f, (GLint, GLfloat))
RGL_FN(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat*))
RGL_FN(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))
RGL_FN(void, glEnableVertexAttribArray, (GLuint))
RGL_FN(void, glDisableVertexAttribArray, (GLuint))
RGL_FN(void, glGenerateMipmap, (GLenum))
#define glCreateShader glCreateShader_
#define glShaderSource glShaderSource_
#define glCompileShader glCompileShader_
#define glGetShaderiv glGetShaderiv_
#define glGetShaderInfoLog glGetShaderInfoLog_
#define glCreateProgram glCreateProgram_
#define glAttachShader glAttachShader_
#define glBindAttribLocation glBindAttribLocation_
#define glLinkProgram glLinkProgram_
#define glGetProgramiv glGetProgramiv_
#define glGetProgramInfoLog glGetProgramInfoLog_
#define glUseProgram glUseProgram_
#define glGetUniformLocation glGetUniformLocation_
#define glUniform1i glUniform1i_
#define glUniform1f glUniform1f_
#define glUniformMatrix4fv glUniformMatrix4fv_
#define glVertexAttribPointer glVertexAttribPointer_
#define glEnableVertexAttribArray glEnableVertexAttribArray_
#define glDisableVertexAttribArray glDisableVertexAttribArray_
#define glGenerateMipmapPtr glGenerateMipmap_
template <class T> static bool load(T& fn, const char* name) {
  fn = (T)wglGetProcAddress(name);
  return fn != 0;
}
static bool loadGL2() {
  bool ok = true;
  ok &= load(glCreateShader_, "glCreateShader");
  ok &= load(glShaderSource_, "glShaderSource");
  ok &= load(glCompileShader_, "glCompileShader");
  ok &= load(glGetShaderiv_, "glGetShaderiv");
  ok &= load(glGetShaderInfoLog_, "glGetShaderInfoLog");
  ok &= load(glCreateProgram_, "glCreateProgram");
  ok &= load(glAttachShader_, "glAttachShader");
  ok &= load(glBindAttribLocation_, "glBindAttribLocation");
  ok &= load(glLinkProgram_, "glLinkProgram");
  ok &= load(glGetProgramiv_, "glGetProgramiv");
  ok &= load(glGetProgramInfoLog_, "glGetProgramInfoLog");
  ok &= load(glUseProgram_, "glUseProgram");
  ok &= load(glGetUniformLocation_, "glGetUniformLocation");
  ok &= load(glUniform1i_, "glUniform1i");
  ok &= load(glUniform1f_, "glUniform1f");
  ok &= load(glUniformMatrix4fv_, "glUniformMatrix4fv");
  ok &= load(glVertexAttribPointer_, "glVertexAttribPointer");
  ok &= load(glEnableVertexAttribArray_, "glEnableVertexAttribArray");
  ok &= load(glDisableVertexAttribArray_, "glDisableVertexAttribArray");
  load(glGenerateMipmap_, "glGenerateMipmap");   // GL 3.0 / ARB_fbo: optional on desktop
  return ok;
}
#else
#define glGenerateMipmapPtr glGenerateMipmap
#endif

namespace rgl {
namespace {

Backend g_backend = FixedFunction;

// ---- ES2 backend state -----------------------------------------------------------------------
struct Vtx { float x, y, z, u, v, r, g, b, a; };
struct St {
  bool tex2d, alphaTest;
  unsigned alphaFunc;
  float alphaRef;
  Tex bound;
  float proj[16], mv[16];
  unsigned mode;
  float cr, cg, cb, ca, tu, tv;
  unsigned prim;
  St() : tex2d(false), alphaTest(false), alphaFunc(GREATER), alphaRef(0.0f), bound(0), mode(MODELVIEW),
         cr(1), cg(1), cb(1), ca(1), tu(0), tv(0), prim(0) {
    for (int i = 0; i < 16; i++) proj[i] = mv[i] = (i % 5 == 0) ? 1.0f : 0.0f;
  }
};
St s;
std::vector<Vtx> g_prim;     // vertices of the primitive between Begin and End
std::vector<Vtx> g_batch;    // triangles waiting for the current GPU state
int g_draws = 0;
GLuint g_prog = 0;
GLint uProj = -1, uTex = -1, uUseTex = -1, uAlphaMode = -1, uAlphaRef = -1;

const char* kVS =
    "attribute vec3 aPos;\n"
    "attribute vec2 aUV;\n"
    "attribute vec4 aCol;\n"
    "uniform mat4 uProj;\n"
    "varying vec2 vUV;\n"
    "varying vec4 vCol;\n"
    "void main() { gl_Position = uProj * vec4(aPos, 1.0); vUV = aUV; vCol = aCol; }\n";
// highp UVs where the GPU has them: tiled walls reach uv ~16, and at mediump (>= 10-bit mantissa)
// that is ~2 texels of error on a 128 px texture -- visible swimming on SGX-class hardware.
const char* kFS =
    "#ifdef GL_ES\n"
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "#endif\n"
    "uniform sampler2D uTex;\n"
    "uniform float uUseTex;\n"
    "uniform float uAlphaMode;\n"   // 0 none, 1 GREATER, 2 GEQUAL (float: ES2-era drivers and int uniforms in branches)
    "uniform float uAlphaRef;\n"
    "varying vec2 vUV;\n"
    "varying vec4 vCol;\n"
    "void main() {\n"
    "  vec4 c = vCol;\n"
    "  if (uUseTex > 0.5) c *= texture2D(uTex, vUV);\n"
    // Fixed-function alpha test compares 8-bit fixed-point values, with the reference truncated
    // (0.5 -> 127/255). A float compare drops SDF glyph edge texels the fixed pipeline keeps:
    // measured 43 differing edge pixels on Secret's "0/7 Notebooks" (LiberationSans SDF).
    "  float qa = floor(c.a * 255.0 + 0.5);\n"
    "  float qr = floor(uAlphaRef * 255.0);\n"
    "  if (uAlphaMode > 0.5 && uAlphaMode < 1.5 && !(qa > qr)) discard;\n"
    "  if (uAlphaMode > 1.5 && !(qa >= qr)) discard;\n"
    "  gl_FragColor = c;\n"
    "}\n";

GLuint compile(GLenum type, const char* src) {
  GLuint sh = glCreateShader(type);
  glShaderSource(sh, 1, &src, 0);
  glCompileShader(sh);
  GLint ok = 0;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[1024] = { 0 };
    glGetShaderInfoLog(sh, sizeof(log), 0, log);
    printf("rgl: shader compile failed: %s\n", log);
    return 0;
  }
  return sh;
}

void mul4(const float* a, const float* b, float* r) {   // r = a * b, column-major
  for (int c = 0; c < 4; c++)
    for (int row = 0; row < 4; row++) {
      float v = 0.0f;
      for (int k = 0; k < 4; k++) v += a[k * 4 + row] * b[c * 4 + k];
      r[c * 4 + row] = v;
    }
}

void drawBatch() {
  if (g_batch.empty()) return;
  glUseProgram(g_prog);
  glUniformMatrix4fv(uProj, 1, GL_FALSE, s.proj);
  const bool useTex = s.tex2d && s.bound != 0;
  glUniform1f(uUseTex, useTex ? 1.0f : 0.0f);
  glUniform1f(uAlphaMode, !s.alphaTest ? 0.0f : (s.alphaFunc == GEQUAL ? 2.0f : 1.0f));
  glUniform1f(uAlphaRef, s.alphaRef);
  const Vtx* v = &g_batch[0];
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), &v->x);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), &v->u);
  glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx), &v->r);
  glDrawArrays(GL_TRIANGLES, 0, (GLsizei)g_batch.size());
  g_draws++;
  g_batch.clear();
}

inline bool es2() { return g_backend == ES2; }

}  // namespace

bool init(Backend b) {
#if defined(RGL_ES2_ONLY)
  b = ES2;
#endif
  g_backend = b;
  if (b != ES2) return true;
#if !defined(RGL_ES2_ONLY)
  if (!loadGL2()) { printf("rgl: GL 2.0 entry points missing\n"); g_backend = FixedFunction; return false; }
#endif
  GLuint vs = compile(GL_VERTEX_SHADER, kVS), fs = compile(GL_FRAGMENT_SHADER, kFS);
  if (!vs || !fs) return false;
  g_prog = glCreateProgram();
  glAttachShader(g_prog, vs);
  glAttachShader(g_prog, fs);
  glBindAttribLocation(g_prog, 0, "aPos");
  glBindAttribLocation(g_prog, 1, "aUV");
  glBindAttribLocation(g_prog, 2, "aCol");
  glLinkProgram(g_prog);
  GLint ok = 0;
  glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[1024] = { 0 };
    glGetProgramInfoLog(g_prog, sizeof(log), 0, log);
    printf("rgl: program link failed: %s\n", log);
    return false;
  }
  uProj = glGetUniformLocation(g_prog, "uProj");
  uTex = glGetUniformLocation(g_prog, "uTex");
  uUseTex = glGetUniformLocation(g_prog, "uUseTex");
  uAlphaMode = glGetUniformLocation(g_prog, "uAlphaMode");
  uAlphaRef = glGetUniformLocation(g_prog, "uAlphaRef");
  glUseProgram(g_prog);
  glUniform1i(uTex, 0);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glEnableVertexAttribArray(2);
  return true;
}

Backend backend() { return g_backend; }

Tex upload(const unsigned char* px, int w, int h, bool point, bool mip) {
  const bool pot = w > 0 && h > 0 && (w & (w - 1)) == 0 && (h & (h - 1)) == 0;
  if (!pot) mip = false;                       // ES 2.0: NPOT textures cannot be mipmapped
  GLuint id = 0;
  glGenTextures(1, &id);
  glBindTexture(GL_TEXTURE_2D, id);
  const GLint wrap = pot ? GL_REPEAT : GL_CLAMP_TO_EDGE;   // ES 2.0: NPOT must clamp
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
  const GLint magF = point ? GL_NEAREST : GL_LINEAR;
  GLint minF = point ? GL_NEAREST : GL_LINEAR;
  bool genAfter = false;
  if (mip) {
    minF = point ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR;
#if !defined(RGL_ES2_ONLY)
    if (!es2() || !glGenerateMipmapPtr) glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    else genAfter = true;
#else
    genAfter = true;
#endif
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minF);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magF);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
  if (genAfter) glGenerateMipmapPtr(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, s.bound);       // the caller's binding is unchanged
  return id;
}

void Enable(unsigned cap) {
  if (!es2()) { glEnable(cap); return; }
  if (cap == TEXTURE_2D) { if (!s.tex2d) { drawBatch(); s.tex2d = true; } return; }
  if (cap == ALPHA_TEST) { if (!s.alphaTest) { drawBatch(); s.alphaTest = true; } return; }
  drawBatch();
  glEnable(cap);
}
void Disable(unsigned cap) {
  if (!es2()) { glDisable(cap); return; }
  if (cap == TEXTURE_2D) { if (s.tex2d) { drawBatch(); s.tex2d = false; } return; }
  if (cap == ALPHA_TEST) { if (s.alphaTest) { drawBatch(); s.alphaTest = false; } return; }
  drawBatch();
  glDisable(cap);
}
void BlendFunc(unsigned a, unsigned b) { drawBatch(); glBlendFunc(a, b); }
void DepthMask(bool on) { drawBatch(); glDepthMask(on ? GL_TRUE : GL_FALSE); }
void DepthFunc(unsigned f) { drawBatch(); glDepthFunc(f); }
void CullFace(unsigned m) { drawBatch(); glCullFace(m); }
void FrontFace(unsigned m) { drawBatch(); glFrontFace(m); }
void AlphaFunc(unsigned f, float ref) {
  if (!es2()) {
#if !defined(RGL_ES2_ONLY)
    glAlphaFunc(f, ref);
#endif
    return;
  }
  if (f != s.alphaFunc || ref != s.alphaRef) { drawBatch(); s.alphaFunc = f; s.alphaRef = ref; }
}
void BindTexture(unsigned target, Tex t) {
  if (!es2()) { glBindTexture(target, t); return; }
  if (t != s.bound) { drawBatch(); s.bound = t; glBindTexture(target, t); }
}
void MatrixMode(unsigned m) {
#if !defined(RGL_ES2_ONLY)
  if (!es2()) { glMatrixMode(m); return; }
#endif
  s.mode = m;
}
void LoadMatrixf(const float* m) {
#if !defined(RGL_ES2_ONLY)
  if (!es2()) { glLoadMatrixf(m); return; }
#endif
  if (s.mode == PROJECTION) {
    if (memcmp(m, s.proj, sizeof(s.proj)) != 0) { drawBatch(); memcpy(s.proj, m, sizeof(s.proj)); }
  } else {
    memcpy(s.mv, m, sizeof(s.mv));             // eye-space vertices: no flush needed
  }
}
void LoadIdentity() {
#if !defined(RGL_ES2_ONLY)
  if (!es2()) { glLoadIdentity(); return; }
#endif
  float id[16];
  for (int i = 0; i < 16; i++) id[i] = (i % 5 == 0) ? 1.0f : 0.0f;
  LoadMatrixf(id);
}
void Ortho(double l, double r, double b, double t, double n, double f) {
#if !defined(RGL_ES2_ONLY)
  if (!es2()) { glOrtho(l, r, b, t, n, f); return; }
#endif
  float o[16];                                 // glOrtho's matrix, then multiplied in like GL does
  for (int i = 0; i < 16; i++) o[i] = 0.0f;
  o[0] = (float)(2.0 / (r - l));
  o[5] = (float)(2.0 / (t - b));
  o[10] = (float)(-2.0 / (f - n));
  o[12] = (float)(-(r + l) / (r - l));
  o[13] = (float)(-(t + b) / (t - b));
  o[14] = (float)(-(f + n) / (f - n));
  o[15] = 1.0f;
  float cur[16], res[16];
  memcpy(cur, s.mode == PROJECTION ? s.proj : s.mv, sizeof(cur));
  mul4(cur, o, res);
  LoadMatrixf(res);
}
void Color4f(float r, float g, float b, float a) {
#if !defined(RGL_ES2_ONLY)
  if (!es2()) { glColor4f(r, g, b, a); return; }
#endif
  s.cr = r; s.cg = g; s.cb = b; s.ca = a;      // per-vertex attribute: never a flush
}
void Begin(unsigned prim) {
#if !defined(RGL_ES2_ONLY)
  if (!es2()) { glBegin(prim); return; }
#endif
  s.prim = prim;
  g_prim.clear();
}
void TexCoord2f(float u, float v) {
#if !defined(RGL_ES2_ONLY)
  if (!es2()) { glTexCoord2f(u, v); return; }
#endif
  s.tu = u; s.tv = v;
}
void Vertex3f(float x, float y, float z) {
#if !defined(RGL_ES2_ONLY)
  if (!es2()) { glVertex3f(x, y, z); return; }
#endif
  const float* m = s.mv;                       // modelview is affine: w stays 1
  Vtx v;
  v.x = m[0] * x + m[4] * y + m[8] * z + m[12];
  v.y = m[1] * x + m[5] * y + m[9] * z + m[13];
  v.z = m[2] * x + m[6] * y + m[10] * z + m[14];
  v.u = s.tu; v.v = s.tv;
  v.r = s.cr; v.g = s.cg; v.b = s.cb; v.a = s.ca;
  g_prim.push_back(v);
}
void Vertex2f(float x, float y) {
#if !defined(RGL_ES2_ONLY)
  if (!es2()) { glVertex2f(x, y); return; }
#endif
  Vertex3f(x, y, 0.0f);
}
void End() {
#if !defined(RGL_ES2_ONLY)
  if (!es2()) { glEnd(); return; }
#endif
  if (s.prim == QUADS) {                       // GL's quad split: (0,1,2) (0,2,3)
    for (size_t i = 0; i + 3 < g_prim.size(); i += 4) {
      const Vtx* q = &g_prim[i];
      g_batch.push_back(q[0]); g_batch.push_back(q[1]); g_batch.push_back(q[2]);
      g_batch.push_back(q[0]); g_batch.push_back(q[2]); g_batch.push_back(q[3]);
    }
  } else {
    for (size_t i = 0; i + 2 < g_prim.size(); i += 3) {
      g_batch.push_back(g_prim[i]); g_batch.push_back(g_prim[i + 1]); g_batch.push_back(g_prim[i + 2]);
    }
  }
  g_prim.clear();
  if (g_batch.size() > 60000) drawBatch();
}
void Viewport(int x, int y, int w, int h) { drawBatch(); glViewport(x, y, w, h); }
void ClearColor(float r, float g, float b, float a) { glClearColor(r, g, b, a); }
void Clear(unsigned bits) { drawBatch(); glClear(bits); }
void Finish() { drawBatch(); glFinish(); }
void ReadPixels(int w, int h, unsigned char* px) {
  drawBatch();
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
}
int flush() {
  drawBatch();
  const int n = g_draws;
  g_draws = 0;
  return n;
}

}  // namespace rgl
