#ifndef PLEZY_LINUX_XR_SCREEN_ADJUST_H_
#define PLEZY_LINUX_XR_SCREEN_ADJUST_H_

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>

#include "screen_geometry.h"

// Letting the viewer move, resize and curve the cinema screen. Pure maths and a
// tiny text format so it can be tested without OpenXR; the renderer feeds it
// controller sticks and saves the result per environment.

namespace xr {

constexpr float kMinDistance = 1.f, kMaxDistance = 30.f;
constexpr float kMinWidth = 1.f, kMaxWidth = 40.f;
constexpr float kMinHeightOffset = -3.f, kMaxHeightOffset = 3.f;

// Per-second rates at full stick deflection. Size and distance scale
// proportionally (a 10 m screen changes faster than a 2 m one), so every step
// feels the same; curvature and height are linear.
constexpr float kScaleRate = 0.9f;
constexpr float kCurvatureRate = 0.7f;
constexpr float kHeightRate = 0.9f;

// Stick values, -1..1, positive = up / right.
struct ScreenAdjustInput {
  float distance = 0.f;   // up = push the screen away
  float width = 0.f;      // up = bigger
  float curvature = 0.f;  // right = more curved
  float height = 0.f;     // up = raise the screen
};

// Ignore drift near the centre and rescale so the live range still reaches 1.
inline float StickDeadzone(float v, float zone = 0.2f) {
  if (!std::isfinite(v)) return 0.f;
  const float a = std::fabs(v);
  if (a <= zone) return 0.f;
  const float scaled = std::min(1.f, (a - zone) / (1.f - zone));
  return v < 0.f ? -scaled : scaled;
}

inline ScreenParams ClampScreen(ScreenParams p) {
  p.distance_m = std::min(std::max(std::isfinite(p.distance_m) ? p.distance_m : 6.f, kMinDistance), kMaxDistance);
  p.width_m = std::min(std::max(std::isfinite(p.width_m) ? p.width_m : 6.f, kMinWidth), kMaxWidth);
  p.curvature = std::min(std::max(std::isfinite(p.curvature) ? p.curvature : 0.f, 0.f), 1.f);
  p.height_offset_m = std::min(
      std::max(std::isfinite(p.height_offset_m) ? p.height_offset_m : 0.f, kMinHeightOffset), kMaxHeightOffset);
  return p;
}

// One frame of adjustment. [dt] is clamped so a long frame cannot fling the screen.
inline ScreenParams AdjustScreen(ScreenParams p, const ScreenAdjustInput& in, float dt) {
  dt = std::min(std::max(dt, 0.f), 0.1f);
  p.distance_m *= std::exp(kScaleRate * StickDeadzone(in.distance) * dt);
  p.width_m *= std::exp(kScaleRate * StickDeadzone(in.width) * dt);
  p.curvature += kCurvatureRate * StickDeadzone(in.curvature) * dt;
  p.height_offset_m += kHeightRate * StickDeadzone(in.height) * dt;
  return ClampScreen(p);
}

inline bool SameScreen(const ScreenParams& a, const ScreenParams& b, float eps = 1e-4f) {
  return std::fabs(a.distance_m - b.distance_m) < eps && std::fabs(a.width_m - b.width_m) < eps &&
         std::fabs(a.curvature - b.curvature) < eps && std::fabs(a.height_offset_m - b.height_offset_m) < eps;
}

// "<environment> <distance> <width> <curvature> <height>", one line per environment.
inline std::string FormatScreenLine(const std::string& environment, const ScreenParams& p) {
  char buf[160];
  std::snprintf(
      buf, sizeof buf, "%s %.3f %.3f %.3f %.3f", environment.c_str(), p.distance_m, p.width_m, p.curvature,
      p.height_offset_m);
  return buf;
}

inline bool ParseScreenLine(const std::string& line, std::string* environment, ScreenParams* out) {
  std::istringstream in(line);
  std::string name;
  float d = 0, w = 0, c = 0, h = 0;
  std::string extra;
  if (!(in >> name >> d >> w >> c >> h) || (in >> extra)) return false;
  if (!std::isfinite(d) || !std::isfinite(w) || !std::isfinite(c) || !std::isfinite(h)) return false;
  ScreenParams p;
  p.distance_m = d;
  p.width_m = w;
  p.curvature = c;
  p.height_offset_m = h;
  *environment = name;
  *out = ClampScreen(p);
  return true;
}

}  // namespace xr

#endif  // PLEZY_LINUX_XR_SCREEN_ADJUST_H_
