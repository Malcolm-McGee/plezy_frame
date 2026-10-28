#ifndef PLEZY_LINUX_XR_SCREEN_GEOMETRY_H_
#define PLEZY_LINUX_XR_SCREEN_GEOMETRY_H_

#include <algorithm>
#include <cmath>
#include <vector>

// The virtual cinema screen: a horizontal arc of a cylinder centred on the
// viewer, so every point is the same distance from the eyes. Pure maths; the
// renderer uploads the vertices it returns.

namespace xr {

struct ScreenVertex {
  float x, y, z;  // metres, viewer at origin, -Z forward, +Y up
  float u, v;     // 0..1 across the whole screen, origin top-left
};

struct ScreenParams {
  float distance_m = 6.f;       // radius of the cylinder
  float width_m = 6.f;          // arc length of the screen
  float aspect = 16.f / 9.f;    // width / height of the picture
  float curvature = 1.f;        // 0 = flat plane, 1 = full cylinder around the viewer
  float height_offset_m = 0.f;  // screen centre relative to eye level
  int columns = 48;             // subdivisions along the arc
};

constexpr float kMinScreenDistance = 0.5f;
constexpr float kMaxScreenWidth = 60.f;

inline float ScreenHeight(const ScreenParams& p) { return p.width_m / std::max(p.aspect, 0.01f); }

// Angle the screen spans as seen from the viewer, in radians.
inline float ScreenArcAngle(const ScreenParams& p) {
  const float d = std::max(p.distance_m, kMinScreenDistance);
  const float w = std::min(std::max(p.width_m, 0.f), kMaxScreenWidth);
  const float flat = 2.f * std::atan(w / (2.f * d));
  const float curved = w / d;
  const float c = std::min(std::max(p.curvature, 0.f), 1.f);
  return flat + (curved - flat) * c;
}

// Width that fills [horizontal_fov_deg] of the view at the given distance.
inline float WidthForFov(float distance_m, float horizontal_fov_deg) {
  const float half = horizontal_fov_deg * 3.14159265f / 360.f;
  return 2.f * std::max(distance_m, kMinScreenDistance) * std::tan(half);
}

// Triangle-strip friendly grid: 2 vertices per column, top then bottom.
inline std::vector<ScreenVertex> BuildScreenStrip(const ScreenParams& p) {
  const int cols = std::max(p.columns, 1);
  const float d = std::max(p.distance_m, kMinScreenDistance);
  const float w = std::min(std::max(p.width_m, 0.f), kMaxScreenWidth);
  const float h = ScreenHeight({d, w, p.aspect, p.curvature, p.height_offset_m, cols});
  const float c = std::min(std::max(p.curvature, 0.f), 1.f);
  const float arc = ScreenArcAngle(p);
  std::vector<ScreenVertex> out;
  out.reserve(static_cast<size_t>(cols + 1) * 2);
  for (int i = 0; i <= cols; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(cols);
    const float s = t - 0.5f;
    // Blend between a flat plane (x linear, z constant) and a cylinder arc.
    const float theta = s * arc;
    const float x_cyl = d * std::sin(theta);
    const float z_cyl = -d * std::cos(theta);
    const float x_flat = s * w;
    const float x = x_flat + (x_cyl - x_flat) * c;
    const float z = -d + (z_cyl + d) * c;
    const float top = p.height_offset_m + h * 0.5f;
    const float bottom = p.height_offset_m - h * 0.5f;
    out.push_back({x, top, z, t, 0.f});
    out.push_back({x, bottom, z, t, 1.f});
  }
  return out;
}

}  // namespace xr

#endif  // PLEZY_LINUX_XR_SCREEN_GEOMETRY_H_
