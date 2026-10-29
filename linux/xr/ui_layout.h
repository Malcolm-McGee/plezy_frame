#ifndef PLEZY_LINUX_XR_UI_LAYOUT_H_
#define PLEZY_LINUX_XR_UI_LAYOUT_H_

#include <algorithm>
#include <cmath>

// Layout and pointing maths for the VR menu. Pure (no GL, no OpenXR) so it can be tested alone.
//
// The menu is drawn into a kPanelWidth x kPanelHeight pixel texture, origin top-left, and that texture is shown
// on a flat quad floating in front of the viewer. Everything here works in panel pixels except the ray maths.

namespace xr {

constexpr float kPanelWidth = 1920.f;
constexpr float kPanelHeight = 1080.f;

struct Rect {
  float x = 0, y = 0, w = 0, h = 0;
  bool Contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

struct GridMetrics {
  float tile_w = 220.f;
  float poster_h = 330.f;  // poster image height; the title and subtitle sit beneath it
  float caption_h = 66.f;  // room for two text lines under the poster
  float gap = 26.f;
  float left = 118.f;
  float top = 170.f;      // top of the scrolling area, below the header
  float bottom = 1050.f;  // bottom of the scrolling area
  int columns = 7;

  float TileH() const { return poster_h + caption_h; }
  float RowPitch() const { return TileH() + gap; }
  float AreaHeight() const { return bottom - top; }
};

inline int RowCount(int tile_count, int columns) { return columns <= 0 ? 0 : (tile_count + columns - 1) / columns; }

// How far the grid can scroll, in pixels (0 when everything fits).
inline float MaxScroll(int tile_count, const GridMetrics& g) {
  const int rows = RowCount(tile_count, g.columns);
  if (rows == 0) return 0.f;
  const float content = rows * g.RowPitch() - g.gap;
  return std::max(0.f, content - g.AreaHeight());
}

inline float ClampScroll(float scroll, int tile_count, const GridMetrics& g) {
  return std::min(std::max(scroll, 0.f), MaxScroll(tile_count, g));
}

// Where tile [index] is drawn with the grid scrolled by [scroll] pixels.
inline Rect TileRect(int index, float scroll, const GridMetrics& g) {
  const int col = index % g.columns;
  const int row = index / g.columns;
  return {g.left + col * (g.tile_w + g.gap), g.top + row * g.RowPitch() - scroll, g.tile_w, g.TileH()};
}

// The tile under panel point (px, py), or -1. Points outside the scrolling area never hit, so a tile
// half-hidden behind the header cannot be picked through it.
inline int HitTile(float px, float py, int tile_count, float scroll, const GridMetrics& g) {
  if (py < g.top || py >= g.bottom) return -1;
  for (int i = 0; i < tile_count; ++i) {
    const Rect r = TileRect(i, scroll, g);
    if (r.y + r.h < g.top || r.y > g.bottom) continue;
    if (r.Contains(px, py)) return i;
  }
  return -1;
}

// The "back" button in the header's right corner.
inline Rect BackButtonRect() { return {kPanelWidth - 118.f - 240.f, 52.f, 240.f, 72.f}; }

struct Vec3 {
  float x = 0, y = 0, z = 0;
};

inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// A flat rectangle in space: centre plus its unit right and up axes, and its size in metres.
struct PanelPlane {
  Vec3 center, right, up;
  float width_m = 3.2f;
  float height_m = 1.8f;
};

struct PanelHit {
  bool hit = false;
  float px = 0, py = 0;  // panel pixels, origin top-left
  float distance = 0;    // metres along the ray
};

// Intersects a ray (origin + t * direction, t > 0) with the panel's front. The panel faces +normal where
// normal = right x up; rays that start behind it or run parallel to it miss.
inline PanelHit RayHitPanel(Vec3 origin, Vec3 dir, const PanelPlane& p) {
  const Vec3 normal{
      p.right.y * p.up.z - p.right.z * p.up.y, p.right.z * p.up.x - p.right.x * p.up.z,
      p.right.x * p.up.y - p.right.y * p.up.x};
  const float denom = Dot(dir, normal);
  if (std::fabs(denom) < 1e-6f) return {};
  const float t = Dot(p.center - origin, normal) / denom;
  if (t <= 0.f) return {};
  const Vec3 hit{origin.x + dir.x * t, origin.y + dir.y * t, origin.z + dir.z * t};
  const Vec3 rel = hit - p.center;
  const float u = Dot(rel, p.right) / p.width_m + 0.5f;  // 0 left .. 1 right
  const float v = 0.5f - Dot(rel, p.up) / p.height_m;    // 0 top .. 1 bottom
  if (u < 0.f || u > 1.f || v < 0.f || v > 1.f) return {};
  return {true, u * kPanelWidth, v * kPanelHeight, t};
}

}  // namespace xr

#endif  // PLEZY_LINUX_XR_UI_LAYOUT_H_
