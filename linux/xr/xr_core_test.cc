// Pure tests for the XR geometry headers; no OpenXR, GL or display needed.
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "environment.h"
#include "screen_adjust.h"
#include "screen_geometry.h"
#include "stereo_layout.h"
#include "ui_layout.h"
#include "xr_math.h"
#include "xr_protocol.h"

static int g_failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)
static bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

int main() {
  using namespace xr;

  // Eye rects
  auto l = EyeUvRect(StereoLayout::kSideBySideHalf, Eye::kLeft, false);
  auto r = EyeUvRect(StereoLayout::kSideBySideHalf, Eye::kRight, false);
  CHECK(l.u0 == 0.f && l.u1 == 0.5f && r.u0 == 0.5f && r.u1 == 1.f);
  auto lsw = EyeUvRect(StereoLayout::kSideBySideFull, Eye::kLeft, true);
  CHECK(lsw.u0 == 0.5f);
  auto t = EyeUvRect(StereoLayout::kTopBottomHalf, Eye::kLeft, false);
  auto b = EyeUvRect(StereoLayout::kTopBottomHalf, Eye::kRight, false);
  CHECK(t.v1 == 0.5f && b.v0 == 0.5f && t.u1 == 1.f);
  auto flat = EyeUvRect(StereoLayout::kNone, Eye::kRight, true);
  CHECK(flat.u0 == 0.f && flat.u1 == 1.f && flat.v0 == 0.f && flat.v1 == 1.f);

  // Display aspect: 1920x1080 HSBS is a 16:9 picture; 3840x1080 FSBS likewise.
  CHECK(Near(EyeDisplayAspect(StereoLayout::kSideBySideHalf, 1920, 1080), 16.f / 9.f));
  CHECK(Near(EyeDisplayAspect(StereoLayout::kSideBySideFull, 3840, 1080), 16.f / 9.f));
  CHECK(Near(EyeDisplayAspect(StereoLayout::kTopBottomHalf, 1920, 1080), 16.f / 9.f));
  CHECK(Near(EyeDisplayAspect(StereoLayout::kTopBottomFull, 1920, 2160), 16.f / 9.f));
  CHECK(Near(EyeDisplayAspect(StereoLayout::kNone, 0, 0), 16.f / 9.f));
  CHECK(StereoLayoutFromWireName("topBottomFull") == StereoLayout::kTopBottomFull);
  CHECK(StereoLayoutFromWireName("junk") == StereoLayout::kNone);

  // Cylinder: every vertex sits `distance` from the viewer in the XZ plane.
  ScreenParams p;
  p.distance_m = 6.f;
  p.width_m = 8.f;
  p.curvature = 1.f;
  auto strip = BuildScreenStrip(p);
  CHECK(strip.size() == static_cast<size_t>(p.columns + 1) * 2);
  for (const auto& v : strip) CHECK(Near(std::sqrt(v.x * v.x + v.z * v.z), 6.f, 1e-3f));
  CHECK(Near(strip.front().u, 0.f) && Near(strip[strip.size() - 2].u, 1.f));
  CHECK(Near(strip[0].y - strip[1].y, ScreenHeight(p)));
  CHECK(strip[0].v == 0.f && strip[1].v == 1.f);

  // Flat: constant depth, linear x.
  p.curvature = 0.f;
  for (const auto& v : BuildScreenStrip(p)) CHECK(Near(v.z, -6.f));
  CHECK(Near(BuildScreenStrip(p).front().x, -4.f));

  // Degenerate input is clamped rather than producing NaN or a zero radius.
  ScreenParams bad;
  bad.distance_m = -3.f;
  bad.width_m = 1e9f;
  bad.columns = 0;
  bad.aspect = 0.f;
  for (const auto& v : BuildScreenStrip(bad)) CHECK(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z));

  CHECK(Near(WidthForFov(6.f, 90.f), 12.f, 1e-3f));

  CHECK(EnvironmentByWireName("lounge").id == EnvironmentId::kLounge);
  CHECK(EnvironmentByWireName("nope").id == EnvironmentId::kCinema);
  CHECK(EnvironmentByWireName("passthrough").passthrough);

  // Maths
  {
    Mat4 pose = PoseMatrix(0.f, std::sin(0.3f), 0.f, std::cos(0.3f), 1.f, 2.f, 3.f);
    Mat4 prod = Multiply(pose, InverseRigid(pose));
    Mat4 id = Identity();
    for (int i = 0; i < 16; ++i) CHECK(Near(prod[i], id[i]));
    Mat4 proj = PerspectiveFromFov(-0.8f, 0.7f, 0.9f, -0.6f, 0.05f, 100.f);
    Mat4 pinv = Multiply(proj, Inverse(proj));
    for (int i = 0; i < 16; ++i) CHECK(Near(pinv[i], id[i], 1e-3f));
    // The centre of the frustum edge at the near plane maps to NDC x = -1.
    const float nx = std::tan(-0.8f) * 0.05f;
    const float clip_x = proj[0] * nx + proj[8] * -0.05f;
    const float clip_w = proj[11] * -0.05f;
    CHECK(Near(clip_x / clip_w, -1.f, 1e-3f));
    Mat4 sing{};
    Mat4 si = Inverse(sing);
    for (int i = 0; i < 16; ++i) CHECK(si[i] == id[i]);
    Mat4 ry = RotationY(3.14159265f / 2.f);  // +Z axis rotates onto +X
    CHECK(Near(ry[8], 1.f) && Near(ry[10], 0.f, 1e-5f));
  }

  // Protocol
  {
    CHECK(ParseCommand("toggle\n").type == CommandType::kToggle);
    CHECK(ParseCommand("seek 12.5").number == 12.5);
    CHECK(ParseCommand("seekrel -10").number == -10.0);
    CHECK(ParseCommand("seek").type == CommandType::kInvalid);
    CHECK(ParseCommand("seek abc").type == CommandType::kInvalid);
    CHECK(ParseCommand("seek nan").type == CommandType::kInvalid);
    CHECK(ParseCommand("volume 250").number == 100.0);
    CHECK(ParseCommand("volume -5").number == 0.0);
    CHECK(ParseCommand("env lounge\r\n").text == "lounge");
    CHECK(ParseCommand("env").type == CommandType::kInvalid);
    CHECK(ParseCommand("layout topBottomHalf").type == CommandType::kLayout);
    CHECK(ParseCommand("rm -rf /").type == CommandType::kInvalid);
    CHECK(ParseCommand("").type == CommandType::kInvalid);
    CHECK(FormatPosition(1.5, 60, true, "void", "none") == "position 1.500 60.000 1 void none no 0");
    CHECK(
        FormatPosition(1.5, 60, false, "cinema", "sideBySideHalf", "v4l2m2m-copy", 12) ==
        "position 1.500 60.000 0 cinema sideBySideHalf v4l2m2m-copy 12");
    CHECK(FormatError("a\nb").find('\n') == std::string::npos);
    // media loading
    CHECK(ParseCommand("load http://h/v.mkv?x=1").text == "http://h/v.mkv?x=1");
    CHECK(ParseCommand("load").type == CommandType::kInvalid);
    CHECK(ParseCommand("header Authorization: Bearer abc").text == "Authorization: Bearer abc");
    CHECK(ParseCommand("start 61.5").number == 61.5);
    CHECK(ParseCommand("start soon").type == CommandType::kInvalid);
    CHECK(ParseCommand("stop").type == CommandType::kStop);
    CHECK(ParseCommand("hwdec software").text == "software");
    CHECK(ParseCommand("hwdec auto").type == CommandType::kHwdec);
    CHECK(ParseCommand("hwdec maybe").type == CommandType::kInvalid);
    // menu screens: tabs separate fields, spaces stay inside them
    Command begin = ParseCommand("ui.begin\tContinue Watching\t1");
    CHECK(
        begin.type == CommandType::kUiBegin && begin.fields.size() == 2 && begin.fields[0] == "Continue Watching" &&
        begin.fields[1] == "1");
    Command tile = ParseCommand("ui.tile\tabc 1\tThe Movie\t2009 PG-13\t/tmp/p 1.jpg\r\n");
    CHECK(
        tile.type == CommandType::kUiTile && tile.fields.size() == 4 && tile.fields[0] == "abc 1" &&
        tile.fields[1] == "The Movie" && tile.fields[3] == "/tmp/p 1.jpg");
    Command bare_tile = ParseCommand("ui.tile\tid\tT\tS\t");  // an empty image path is allowed
    CHECK(bare_tile.type == CommandType::kUiTile && bare_tile.fields.size() == 4 && bare_tile.fields[3].empty());
    CHECK(ParseCommand("ui.tile\tonly\ttwo").type == CommandType::kInvalid);
    CHECK(ParseCommand("ui.begin\tTitle").type == CommandType::kInvalid);
    CHECK(ParseCommand("ui.end").type == CommandType::kUiEnd);
    CHECK(ParseCommand("ui.status\tLoading...").fields[0] == "Loading...");
    CHECK(ParseCommand("ui.bogus\tx").type == CommandType::kInvalid);
    CHECK(ParseCommand("ui.dump\t/tmp/a b.png").fields[0] == "/tmp/a b.png");
    CHECK(ParseCommand("ui.dump").type == CommandType::kInvalid);
    CHECK(FormatSelect("lib 3") == "select\tlib 3");
  }

  // Menu layout and pointing
  {
    GridMetrics g;
    CHECK(RowCount(0, 7) == 0 && RowCount(7, 7) == 1 && RowCount(8, 7) == 2);
    CHECK(MaxScroll(7, g) == 0.f);   // one row fits
    CHECK(MaxScroll(14, g) == 0.f);  // two rows fit too
    CHECK(MaxScroll(15, g) > 0.f);   // a third row does not
    CHECK(ClampScroll(-50.f, 30, g) == 0.f);
    CHECK(Near(ClampScroll(1e9f, 30, g), MaxScroll(30, g)));
    Rect first = TileRect(0, 0.f, g);
    Rect second = TileRect(1, 0.f, g);
    Rect eighth = TileRect(7, 0.f, g);
    CHECK(Near(second.x - first.x, g.tile_w + g.gap));
    CHECK(Near(eighth.x, first.x) && Near(eighth.y - first.y, g.RowPitch()));
    // Every tile is hit at its own centre, and nothing is hit in a gap.
    for (int i = 0; i < 14; ++i) {
      Rect r = TileRect(i, 0.f, g);
      if (r.y + r.h > g.bottom) continue;
      CHECK(HitTile(r.x + r.w / 2, r.y + r.h / 2, 14, 0.f, g) == i);
    }
    CHECK(HitTile(first.x + first.w + g.gap / 2, first.y + 10.f, 14, 0.f, g) == -1);
    // Scrolling moves tiles up, and a tile under the header cannot be picked.
    CHECK(Near(TileRect(0, 100.f, g).y, first.y - 100.f));
    CHECK(HitTile(first.x + 10.f, g.top - 5.f, 14, 0.f, g) == -1);
    CHECK(HitTile(first.x + 10.f, g.top + 5.f, 21, 300.f, g) == 0);  // row 0's caption is still visible
    CHECK(HitTile(first.x + 10.f, g.top + 5.f, 21, 450.f, g) == 7);  // row 0 has scrolled away
    CHECK(BackButtonRect().Contains(kPanelWidth - 200.f, 80.f));

    // A panel 3 m ahead (-Z), 3.2 x 1.8 m, facing the viewer at the origin.
    PanelPlane plane;
    plane.center = {0, 0, -3.f};
    plane.right = {1, 0, 0};
    plane.up = {0, 1, 0};
    PanelHit centre = RayHitPanel({0, 0, 0}, {0, 0, -1}, plane);
    CHECK(centre.hit && Near(centre.px, kPanelWidth / 2, 0.5f) && Near(centre.py, kPanelHeight / 2, 0.5f));
    CHECK(Near(centre.distance, 3.f));
    PanelHit top_left = RayHitPanel({-1.59f, 0.89f, 0}, {0, 0, -1}, plane);
    CHECK(top_left.hit && top_left.px < 20.f && top_left.py < 20.f);
    CHECK(!RayHitPanel({0, 0, 0}, {0, 0, 1}, plane).hit);      // pointing away
    CHECK(!RayHitPanel({0, 0, 0}, {1, 0, 0}, plane).hit);      // parallel to the panel
    CHECK(!RayHitPanel({0, 0, -5.f}, {0, 0, -1}, plane).hit);  // starting behind it
    CHECK(!RayHitPanel({2.f, 0, 0}, {0, 0, -1}, plane).hit);   // beside it
  }

  // Screen adjustment
  {
    ScreenParams base;
    base.distance_m = 6.f;
    base.width_m = 8.f;
    base.curvature = 0.4f;
    base.height_offset_m = 0.f;
    // Deadzone: small drift does nothing, full deflection reaches 1, NaN is ignored.
    CHECK(StickDeadzone(0.15f) == 0.f);
    CHECK(Near(StickDeadzone(1.f), 1.f));
    CHECK(Near(StickDeadzone(-1.f), -1.f));
    CHECK(StickDeadzone(NAN) == 0.f);
    CHECK(StickDeadzone(0.6f) > 0.f && StickDeadzone(0.6f) < 1.f);
    // No input: unchanged.
    CHECK(SameScreen(AdjustScreen(base, {}, 0.016f), base));
    // Each axis moves the right way.
    ScreenAdjustInput up;
    up.width = 1.f;
    CHECK(AdjustScreen(base, up, 0.05f).width_m > base.width_m);
    up.width = -1.f;
    CHECK(AdjustScreen(base, up, 0.05f).width_m < base.width_m);
    ScreenAdjustInput far_in;
    far_in.distance = 1.f;
    CHECK(AdjustScreen(base, far_in, 0.05f).distance_m > base.distance_m);
    ScreenAdjustInput curve;
    curve.curvature = 1.f;
    CHECK(AdjustScreen(base, curve, 0.05f).curvature > base.curvature);
    ScreenAdjustInput raise;
    raise.height = 1.f;
    CHECK(AdjustScreen(base, raise, 0.05f).height_offset_m > 0.f);
    // Scaling is proportional, not additive.
    ScreenAdjustInput grow;
    grow.width = 1.f;
    ScreenParams small = base, big = base;
    small.width_m = 2.f;
    big.width_m = 20.f;
    CHECK(Near((AdjustScreen(big, grow, 0.05f).width_m / 20.f), (AdjustScreen(small, grow, 0.05f).width_m / 2.f)));
    // Holding a stick for a long time stops at the limits.
    ScreenParams p = base;
    ScreenAdjustInput all;
    all.distance = 1.f;
    all.width = 1.f;
    all.curvature = 1.f;
    all.height = 1.f;
    for (int i = 0; i < 2000; ++i) p = AdjustScreen(p, all, 0.016f);
    CHECK(Near(p.distance_m, kMaxDistance) && Near(p.width_m, kMaxWidth));
    CHECK(Near(p.curvature, 1.f) && Near(p.height_offset_m, kMaxHeightOffset));
    for (int i = 0; i < 2000; ++i) p = AdjustScreen(p, {-1.f, -1.f, -1.f, -1.f}, 0.016f);
    CHECK(Near(p.distance_m, kMinDistance) && Near(p.width_m, kMinWidth));
    CHECK(Near(p.curvature, 0.f) && Near(p.height_offset_m, kMinHeightOffset));
    // A huge or negative frame time cannot fling the screen.
    CHECK(Near(AdjustScreen(base, all, 1e6f).width_m, AdjustScreen(base, all, 0.1f).width_m));
    CHECK(SameScreen(AdjustScreen(base, all, -5.f), base));
    // The adjusted screen still builds finite geometry.
    for (const auto& v : BuildScreenStrip(p)) CHECK(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z));
    // Persistence format round-trips and rejects garbage.
    std::string env;
    ScreenParams parsed;
    CHECK(ParseScreenLine(FormatScreenLine("lounge", base), &env, &parsed));
    CHECK(env == "lounge" && SameScreen(parsed, base, 1e-3f));
    CHECK(!ParseScreenLine("lounge 1 2 3", &env, &parsed));
    CHECK(!ParseScreenLine("lounge 1 2 3 4 5", &env, &parsed));
    CHECK(!ParseScreenLine("lounge a b c d", &env, &parsed));
    CHECK(!ParseScreenLine("", &env, &parsed));
    CHECK(!ParseScreenLine("lounge nan 2 3 4", &env, &parsed));
    CHECK(
        ParseScreenLine("void 999 999 9 99", &env, &parsed) && parsed.distance_m == kMaxDistance &&
        parsed.width_m == kMaxWidth && parsed.curvature == 1.f && parsed.height_offset_m == kMaxHeightOffset);
  }

  if (g_failures) return 1;
  std::puts("xr_core_test: all passed");
  return 0;
}
