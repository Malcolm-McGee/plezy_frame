#ifndef PLEZY_LINUX_XR_XR_MATH_H_
#define PLEZY_LINUX_XR_XR_MATH_H_

#include <array>
#include <cmath>

// Minimal column-major 4x4 maths for the XR renderer. OpenXR/GL free so it can
// be tested alone; the renderer converts XrPosef/XrFovf into these.

namespace xr {

using Mat4 = std::array<float, 16>;  // column-major, m[col * 4 + row]

inline Mat4 Identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

inline Mat4 Multiply(const Mat4& a, const Mat4& b) {
  Mat4 r{};
  for (int c = 0; c < 4; ++c)
    for (int row = 0; row < 4; ++row) {
      float s = 0.f;
      for (int k = 0; k < 4; ++k) s += a[k * 4 + row] * b[c * 4 + k];
      r[c * 4 + row] = s;
    }
  return r;
}

inline Mat4 Translation(float x, float y, float z) {
  Mat4 m = Identity();
  m[12] = x;
  m[13] = y;
  m[14] = z;
  return m;
}

inline Mat4 RotationY(float radians) {
  Mat4 m = Identity();
  const float c = std::cos(radians), s = std::sin(radians);
  m[0] = c;
  m[8] = s;
  m[2] = -s;
  m[10] = c;
  return m;
}

// Asymmetric frustum from OpenXR-style half-angle tangents (angles in radians,
// left/down negative), GL clip space (-1..1 depth).
inline Mat4 PerspectiveFromFov(
    float angle_left, float angle_right, float angle_up, float angle_down, float near_z, float far_z) {
  const float l = std::tan(angle_left), r = std::tan(angle_right);
  const float u = std::tan(angle_up), d = std::tan(angle_down);
  const float w = r - l, h = u - d;
  Mat4 m{};
  m[0] = 2.f / w;
  m[5] = 2.f / h;
  m[8] = (r + l) / w;
  m[9] = (u + d) / h;
  m[10] = -(far_z + near_z) / (far_z - near_z);
  m[11] = -1.f;
  m[14] = -(2.f * far_z * near_z) / (far_z - near_z);
  return m;
}

// World-from-eye rigid transform for a pose (unit quaternion x,y,z,w + position).
inline Mat4 PoseMatrix(float qx, float qy, float qz, float qw, float px, float py, float pz) {
  Mat4 m = Identity();
  m[0] = 1 - 2 * (qy * qy + qz * qz);
  m[1] = 2 * (qx * qy + qz * qw);
  m[2] = 2 * (qx * qz - qy * qw);
  m[4] = 2 * (qx * qy - qz * qw);
  m[5] = 1 - 2 * (qx * qx + qz * qz);
  m[6] = 2 * (qy * qz + qx * qw);
  m[8] = 2 * (qx * qz + qy * qw);
  m[9] = 2 * (qy * qz - qx * qw);
  m[10] = 1 - 2 * (qx * qx + qy * qy);
  m[12] = px;
  m[13] = py;
  m[14] = pz;
  return m;
}

// Inverse of a rigid transform (rotation + translation only).
inline Mat4 InverseRigid(const Mat4& m) {
  Mat4 r = Identity();
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r[j * 4 + i] = m[i * 4 + j];  // transpose rotation
  for (int i = 0; i < 3; ++i) r[12 + i] = -(r[0 + i] * m[12] + r[4 + i] * m[13] + r[8 + i] * m[14]);
  return r;
}

// General 4x4 inverse (cofactor expansion). Returns identity for a singular matrix.
inline Mat4 Inverse(const Mat4& m) {
  Mat4 inv{};
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
           m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
           m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
           m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
            m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
           m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
           m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
           m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
            m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] -
           m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
           m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
            m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
            m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] +
           m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] -
           m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] +
            m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] -
            m[8] * m[2] * m[5];
  const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (std::fabs(det) < 1e-12f) return Identity();
  const float inv_det = 1.f / det;
  for (float& v : inv) v *= inv_det;
  return inv;
}

}  // namespace xr

#endif  // PLEZY_LINUX_XR_XR_MATH_H_
