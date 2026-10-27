// render-gl.h -- the renderer's only window onto the GPU.
//
// WHY THIS LAYER EXISTS: the iOS build targets iOS 6, whose GPUs (PowerVR SGX535/543) are driven
// through OpenGL ES 2.0. ES 2.0 has NO fixed-function pipeline: no glBegin/glEnd, no matrix
// stack, no glAlphaFunc, no GL_QUADS, no glEnable(GL_TEXTURE_2D). The Windows testbed was written
// against exactly those. rgl keeps the call SHAPE the passes were written in (so render-scene.cpp
// reads like the original passes) and maps it onto one of two backends:
//
//   Backend::FixedFunction -- the original GL 1.1 calls, verbatim. Windows only. Kept so the
//                             ES2 backend can be pixel-compared against the output that the
//                             earlier sessions verified against real screenshots.
//   Backend::ES2           -- only the GLES 2.0 / desktop GL 2.0 common subset: one shader pair,
//                             client-side vertex arrays, CPU-built matrices, alpha test as a
//                             shader `discard`, quads split into triangles. Vertices are moved to
//                             eye space on the CPU so consecutive draws that share GPU state merge
//                             into one glDrawArrays (the level is ~2600 quads; SGX-era drivers do
//                             not survive thousands of draw calls per frame).
//
// No GL header is included here: render-scene.cpp must compile without one, so any GL call that
// bypasses this layer is a compile error rather than a silent iOS breakage.
#pragma once

namespace rgl {

typedef unsigned int Tex;   // a GL texture name; 0 = none

// Constants: numerically the GL enums, so the FixedFunction backend passes them straight through.
enum {
  TEXTURE_2D = 0x0DE1, BLEND = 0x0BE2, ALPHA_TEST = 0x0BC0, DEPTH_TEST = 0x0B71, CULL_FACE = 0x0B44,
  SRC_ALPHA = 0x0302, ONE_MINUS_SRC_ALPHA = 0x0303,
  GREATER = 0x0204, GEQUAL = 0x0206, LEQUAL = 0x0203,
  MODELVIEW = 0x1700, PROJECTION = 0x1701,
  TRIANGLES = 0x0004, QUADS = 0x0007,
  BACK = 0x0405, CCW = 0x0901,
  COLOR_BUFFER_BIT = 0x4000, DEPTH_BUFFER_BIT = 0x0100
};
enum Backend { FixedFunction = 0, ES2 = 1 };

// Call once with a current GL context. ES2 compiles the shaders (false + a message on failure).
bool init(Backend b);
Backend backend();

// Textures. upload() applies the GLES 2.0 texture rules on EVERY backend so Windows shows what an
// iOS 6 device will: power-of-two -> REPEAT (+ mipmaps when the asset asks); non-power-of-two ->
// CLAMP_TO_EDGE and no mipmaps (an NPOT texture with REPEAT or mips is INCOMPLETE on ES 2.0 and
// samples black). Measured against the bake: no NPOT texture is tiled and every baked-mesh UV is
// inside 0..1, so the clamp never changes a texel this game samples.
Tex upload(const unsigned char* rgba, int w, int h, bool point, bool mip);

// Fixed-function-shaped state + immediate mode (see the header comment).
void Enable(unsigned cap);
void Disable(unsigned cap);
void BlendFunc(unsigned s, unsigned d);
void DepthMask(bool on);
void DepthFunc(unsigned f);
void CullFace(unsigned m);
void FrontFace(unsigned m);
void AlphaFunc(unsigned f, float ref);
void BindTexture(unsigned target, Tex t);
void MatrixMode(unsigned m);
void LoadMatrixf(const float* m16);     // column-major
void LoadIdentity();
void Ortho(double l, double r, double b, double t, double n, double f);
void Color4f(float r, float g, float b, float a);
void Begin(unsigned prim);              // TRIANGLES or QUADS
void TexCoord2f(float u, float v);
void Vertex3f(float x, float y, float z);
void Vertex2f(float x, float y);
void End();
void Viewport(int x, int y, int w, int h);
void ClearColor(float r, float g, float b, float a);
void Clear(unsigned bits);
void Finish();
void ReadPixels(int w, int h, unsigned char* rgbaBottomUp);   // RGBA8, GL row order

// End of frame: submits any batched geometry. Returns GL draw calls issued since the last call.
int flush();

}  // namespace rgl
