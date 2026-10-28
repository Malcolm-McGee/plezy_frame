// Pure tests for the XR geometry headers; no OpenXR, GL or display needed.
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "environment.h"
#include "screen_geometry.h"
#include "stereo_layout.h"
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
    CHECK(FormatPosition(1.5, 60, true, "void", "none") == "position 1.500 60.000 1 void none");
    CHECK(FormatError("a\nb").find('\n') == std::string::npos);
  }

  if (g_failures) return 1;
  std::puts("xr_core_test: all passed");
  return 0;
}
