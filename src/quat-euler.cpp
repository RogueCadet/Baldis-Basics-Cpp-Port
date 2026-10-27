// quat-euler.cpp  -  Unity-exact Quaternion <-> Euler (degrees) conversions.
// FROM-euler is Unity's reference formula verbatim; numerically verified (probe-euler-order.js,
// 2026-10-22 + 2026-10-23) that it composes as q = qz(q_z) * qy * qx  -  the rotation matrix is
// R = Rz Ry Rx, so the to-euler extraction decomposes THAT matrix.
//
// TO-euler (eulerAngles, rewritten 2026-10-23) reproduces Unity's OBSERVED native read-back. Unity
// applies Mathf.Rad2Deg to an internal ToEulerRad, then Internal_MakePositive (UnityCsReference
// verbatim, thresholds kept below), so the result is in [0,360). The raw extraction is the ZYX
// decomposition (middle angle = yaw; beta via atan2 against the |cos(y)| hypot - see below) PLUS
// two Unity branch choices, both pinned by
// Unity's own published/observed outputs and - decisively - by the real game: Secret's camera
// (rot (0,1,0,0) = 180 deg about Y) must read back with pitch 0, because PlayerScript::MouseMove
// re-applies (eulerAngles.x, eulerAngles.y, fliparoo) every frame and the real game stays upright.
//     1) ZYX gimbal fold: at yaw +-90 deg only the combination alpha-gamma (+alpha+gamma at -90)
//        survives; fold roll z := 0 and pitch := atan2f(-R12, R11). Round-trip exact, and an
//        authored z=0 composed rotation such as Euler(30,90,0) reads back as (30,90,0).
//     2) PITCH CLAMP: if the pitch angle leaves [-90,90], swap to the alternate Tait-Bryan tuple
//        (alpha -+ 180, 180 - beta, gamma -+ 180) - the unique 90-fold member with pitch in range.
//        This is what makes (0,1,0,0) read (0,180,0) and (1,0,0,0) read (0,180,180) (Unity's
//        famous quirk: a (180,0,0) X rotation re-expresses itself as 0/180/180).
// Observed/read-back table reproduced everywhere (probe-euler-fix.js, ALL GREEN):
//     (0,1,0,0) -> (0,180,0)     Secret; MouseMove stability required
//     (1,0,0,0) -> (0,180,180)   Unity quirk
//     (0,0,1,0) -> (0,0,180)     Rx(90) -> (90,0,0)     Rz(-90) -> (0,0,270)
//     Ry(-10)   -> (0,350,0)     Ry(105) -> (0,105,0)   Rx(-90) -> (270,0,0)
//     (30,90,0)->(30,90,0) gimbal fold    (179,359,1)->(359,181,181) alternate-tuple form
#include "bbmath.h"
#include <cmath>

namespace bb {

Vec3 Quat::eulerAngles() const {
  // Standard row-major rotation matrix from the quaternion (same convention as the from-euler).
  const float wx = w * x, wy = w * y, wz = w * z;
  const float xx = x * x, xy = x * y, xz = x * z;
  const float yy = y * y, yz = y * z, zz = z * z;
  const float R00 = 1.0f - 2.0f * (yy + zz);
  const float R10 = 2.0f * (xy + wz);
  const float R11 = 1.0f - 2.0f * (xx + zz);
  const float R12 = 2.0f * (yz - wx);
  const float R20 = 2.0f * (xz - wy);
  const float R21 = 2.0f * (yz + wx);
  const float R22 = 1.0f - 2.0f * (xx + yy);

  const float kHalfPi  = 1.5707963267948966f;
  const float kPi      = 3.1415926535897932f;
  const float kRad2Deg = 57.2957795f;

  // ZYX extraction, ALL angles via atan2 (never asin: asin's sqrt(eps) blowup near +-1 would sag
  // a 90 deg yaw to 89.98). Row facts used: R20 = -sin(y) exactly; R00/R10 both carry cos(y)
  // (R00 = cz*cy, R10 = sz*cy), so their hypot is |cos(y)| and
  //     beta = atan2(-R20, hypot(R00, R10))
  // is exact and linear-sensitive right up to the gimbal (Ry(90) reads back 90.000000).
  float beta = atan2f(-R20, sqrtf(R00 * R00 + R10 * R10));
  float alpha, gamma;
  if (1.0f - fabsf(-R20) < 1e-6f) {
    // ZYX gimbal (yaw ~ +-90: alpha and gamma degenerate into one combination; R21/R22 then carry
    // only cancellation noise and must not feed atan2f). Only alpha-gamma (+alpha+gamma at -90)
    // survives, and atan2f(-R12, R11) recovers the combination for BOTH signs (rows give
    // m12 = -sin(d), m11 = cos(d)). Fold roll z := 0 so the tuple stays MouseMove-stable, and an
    // authored z=0 composed rotation such as Euler(30,90,0) reads back as exactly (30,90,0).
    alpha = atan2f(-R12, R11);
    gamma = 0.0f;
  } else {
    alpha = atan2f(R21, R22);                                  // pitch (ratio: cy cancels)
    gamma = atan2f(R10, R00);                                  // roll  (ratio: cy cancels)
  }
  // Pitch clamp: keep the 90-fold whose member has pitch in [-90,90] (Unity branch choice; the
  // (alpha,180-beta,gamma) shifted member decodes the exact same rotation - identity is exact).
  if (alpha > kHalfPi) {
    alpha -= kPi;  beta = kPi - beta;  gamma -= kPi;
  } else if (alpha < -kHalfPi) {
    alpha += kPi;  beta = kPi - beta;  gamma += kPi;
  }

  // Internal_MakePositive verbatim (UnityCsReference): [0,360) with the -0.0001 deg old-behaviour
  // hack at both ends.
  Vec3 deg(alpha * kRad2Deg, beta * kRad2Deg, gamma * kRad2Deg);
  const float negativeFlip = -0.0001f * kRad2Deg;   // == -0.00572958 deg
  const float positiveFlip = 360.0f + negativeFlip; // == 359.99427042 deg
  if (deg.x < negativeFlip)        deg.x += 360.0f;
  else if (deg.x > positiveFlip)   deg.x -= 360.0f;
  if (deg.y < negativeFlip)        deg.y += 360.0f;
  else if (deg.y > positiveFlip)   deg.y -= 360.0f;
  if (deg.z < negativeFlip)        deg.z += 360.0f;
  else if (deg.z > positiveFlip)   deg.z -= 360.0f;
  return deg;
}

void Quat::setEulerAngles(const Vec3& deg) {
  *this = Quat::euler(deg);
}

Quat Quat::euler(const Vec3& deg) {
  // Unity reference formula (sx*cy*cz form); degrees -> half-angle factors.
  const float kDeg2Rad = 0.017453292519943295f;
  const float hx = deg.x * kDeg2Rad * 0.5f;
  const float hy = deg.y * kDeg2Rad * 0.5f;
  const float hz = deg.z * kDeg2Rad * 0.5f;
  const float cx = cosf(hx), sx = sinf(hx);
  const float cy = cosf(hy), sy = sinf(hy);
  const float cz = cosf(hz), sz = sinf(hz);
  return Quat(
    sx * cy * cz - cx * sy * sz,
    cx * sy * cz + sx * cy * sz,
    cx * cy * sz - sx * sy * cz,
    cx * cy * cz + sx * sy * sz);
}

} // namespace bb
