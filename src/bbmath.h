// math.h  -  minimal math for the Baldi C++ port. C++11, header-only.
// Target: Apple LLVM 5.1 (Xcode 5.1.1) and MSVC/MinGW for the Windows testbed.
#pragma once
#include <cmath>

namespace bb {

const float kDeg2Rad = 0.01745329251994329576923690768489f;
const float kEps     = 1e-8f;

struct Vec2 {
  float x = 0, y = 0;
  Vec2() {}
  Vec2(float x_, float y_) : x(x_), y(y_) {}
  Vec2 operator+(const Vec2& o) const { return Vec2(x + o.x, y + o.y); }
  Vec2 operator-(const Vec2& o) const { return Vec2(x - o.x, y - o.y); }
  Vec2 operator*(float s) const { return Vec2(x * s, y * s); }
};

struct Vec3 {
  float x = 0, y = 0, z = 0;
  Vec3() {}
  Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
  static Vec3 zero() { return Vec3(0, 0, 0); }
  static Vec3 one()  { return Vec3(1, 1, 1); }
  Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
  Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
  Vec3 operator-() const { return Vec3(-x, -y, -z); }
  Vec3 operator*(float s) const { return Vec3(x * s, y * s, z * s); }
  Vec3 operator/(float s) const { float inv = 1.0f / s; return Vec3(x * inv, y * inv, z * inv); }
  Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
  Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
  Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
  bool operator==(const Vec3& o) const { return x == o.x && y == o.y && z == o.z; }
  bool operator!=(const Vec3& o) const { return !(*this == o); }
  float magnitude() const { return sqrtf(x * x + y * y + z * z); } // Unity Vector3.magnitude
};

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3  cross(const Vec3& a, const Vec3& b) {
  return Vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
inline float length(const Vec3& v) { return sqrtf(dot(v, v)); }
inline float dist(const Vec3& a, const Vec3& b) { return length(b - a); } // Vector3.Distance
inline Vec3  normalized(const Vec3& v) {
  float l = length(v);
  return l > kEps ? v / l : Vec3::zero();
}
inline Vec3  lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float moveTowards(float current, float target, float maxDelta) {
  if (fabsf(target - current) <= maxDelta) return target;
  return current + (target > current ? maxDelta : -maxDelta);
}
// Vector3.MoveTowards
inline Vec3 moveTowards(const Vec3& current, const Vec3& target, float maxDelta) {
  float d = dist(target, current);
  if (d <= maxDelta || d < kEps) return target;
  return current + (target - current) * (maxDelta / d);
}

struct Quat {
  float x = 0, y = 0, z = 0, w = 1; // Unity component order
  Quat() {}
  Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
  Quat operator*(const Quat& o) const {
    return Quat(
      w * o.x + x * o.w + y * o.z - z * o.y,
      w * o.y - x * o.z + y * o.w + z * o.x,
      w * o.z + x * o.y - y * o.x + z * o.w,
      w * o.w - x * o.x - y * o.y - z * o.z);
  }
  static Quat identity() { return Quat(0, 0, 0, 1); }
  static Quat axisAngle(const Vec3& axis, float deg) {
    float h = deg * kDeg2Rad * 0.5f;
    float s = sinf(h);
    return Quat(axis.x * s, axis.y * s, axis.z * s, cosf(h));
  }
  Vec3 rotate(const Vec3& v) const { // q * v * q^-1
    Vec3 q(x, y, z);
    Vec3 t = cross(q, v) * 2.0f;
    return v + t * w + cross(q, t);
  }
  Vec3 eulerAngles() const;             // engine impl: Unity eulerAngles (degrees); Phase 2 .cpp
  void setEulerAngles(const Vec3& deg); // engine impl: Quaternion.Euler equivalent; Phase 2 .cpp
  static Quat euler(const Vec3& deg);   // engine impl: static Quaternion.Euler factory (Phase 2;
                                        // same conversion as setEulerAngles, fresh value)
};

// componentwise scale helpers (TRS composition; callers pre-check for zero scales)
inline Vec3 compMul(const Vec3& a, const Vec3& b) { return Vec3(a.x * b.x, a.y * b.y, a.z * b.z); }
inline Vec3 compDiv(const Vec3& a, const Vec3& b) { // guard-free (callers pre-check scales)
  return Vec3(a.x / b.x, a.y / b.y, a.z / b.z);
}
// quaternion conjugate (inverse for unit rotations)
inline Quat conj(const Quat& q) { return Quat(-q.x, -q.y, -q.z, q.w); }

// column-major 4x4, OpenGL/GLKit-compatible
struct Mat4 {
  float m[16];
  Mat4() { for (int i = 0; i < 16; i++) m[i] = 0.0f; m[0] = m[5] = m[10] = m[15] = 1.0f; }
  static Mat4 identity() { return Mat4(); }
  static Mat4 perspective(float degFovY, float aspect, float zNear, float zFar) {
    Mat4 r; r.m[0] = r.m[5] = r.m[15] = 0.0f; r.m[10] = 0.0f;
    float f = 1.0f / tanf(degFovY * kDeg2Rad * 0.5f);
    r.m[0]  = f / aspect;
    r.m[5]  = f;
    r.m[10] = (zFar + zNear) / (zNear - zFar);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f * zFar * zNear) / (zNear - zFar);
    return r;
  }
  static Mat4 lookAt(const Vec3& eye, const Vec3& center, const Vec3& up) {
    Mat4 r;
    Vec3 f = normalized(center - eye);
    Vec3 s = normalized(cross(f, up));
    Vec3 u = cross(s, f);
    r.m[0] =  s.x; r.m[4] =  s.y; r.m[8]  =  s.z;
    r.m[1] =  u.x; r.m[5] =  u.y; r.m[9]  =  u.z;
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
    r.m[12] = -dot(s, eye);
    r.m[13] = -dot(u, eye);
    r.m[14] =  dot(f, eye);
    return r;
  }
  static Mat4 translate(const Vec3& t) {
    Mat4 r; r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z; return r;
  }
  Mat4 operator*(const Mat4& o) const {
    Mat4 r;
    for (int c = 0; c < 4; c++)
      for (int row = 0; row < 4; row++) {
        float a = 0;
        for (int k = 0; k < 4; k++) a += m[k * 4 + row] * o.m[c * 4 + k];
        r.m[c * 4 + row] = a;
      }
    return r;
  }
  Vec3 transformPoint(const Vec3& p) const {
    return Vec3(
      m[0] * p.x + m[4] * p.y + m[8]  * p.z + m[12],
      m[1] * p.x + m[5] * p.y + m[9]  * p.z + m[13],
      m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]);
  }
  Vec3 transformDir(const Vec3& d) const {
    return Vec3(m[0] * d.x + m[4] * d.y + m[8]  * d.z,
                m[1] * d.x + m[5] * d.y + m[9]  * d.z,
                m[2] * d.x + m[6] * d.y + m[10] * d.z);
  }
};

struct AABB {
  Vec3 min, max;
  AABB() {}
  AABB(const Vec3& mn, const Vec3& mx) : min(mn), max(mx) {}
  bool intersects(const AABB& o) const {
    return min.x <= o.max.x && max.x >= o.min.x &&
           min.y <= o.max.y && max.y >= o.min.y &&
           min.z <= o.max.z && max.z >= o.min.z;
  }
  bool contains(const Vec3& p) const {
    return p.x >= min.x && p.x <= max.x &&
           p.y >= min.y && p.y <= max.y &&
           p.z >= min.z && p.z <= max.z;
  }
};

struct Plane { // n * x + d = 0
  Vec3 n; float d = 0;
  float side(const Vec3& p) const { return dot(n, p) + d; }
};

} // namespace bb
