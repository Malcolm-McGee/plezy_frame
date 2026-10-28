#ifndef PLEZY_LINUX_XR_ENVIRONMENT_H_
#define PLEZY_LINUX_XR_ENVIRONMENT_H_

#include <array>
#include <cstddef>
#include <string>

// The built-in viewing environments. They are procedural, with no assets to
// ship: the renderer draws a sky gradient plus a few lit surfaces from these
// parameters. kPassthrough shows the real room instead of drawing anything.

namespace xr {

enum class EnvironmentId { kVoid, kCinema, kLounge, kSpace, kPassthrough };

struct Rgb {
  float r, g, b;
};

struct EnvironmentSpec {
  EnvironmentId id;
  const char* wire_name;
  const char* label;
  bool passthrough;       // composite over the real world, draw nothing else
  Rgb sky_top;            // gradient zenith
  Rgb sky_horizon;        // gradient horizon
  Rgb floor;              // floor colour, ignored when has_floor is false
  bool has_floor;
  float ambient_bleed;    // 0..1: how much screen average colour lights the room
  float screen_distance;  // preferred screen distance, metres
  float screen_width;     // preferred screen width, metres
  float screen_curvature; // 0 flat .. 1 cylinder
};

inline constexpr std::array<EnvironmentSpec, 5> kEnvironments = {{
    {EnvironmentId::kVoid, "void", "Void", false, {0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}, false, 0.f, 4.f,
     3.2f, 0.f},
    {EnvironmentId::kCinema, "cinema", "Cinema", false, {0.02f, 0.02f, 0.03f}, {0.05f, 0.04f, 0.05f},
     {0.10f, 0.03f, 0.04f}, true, 0.6f, 9.f, 10.f, 0.4f},
    {EnvironmentId::kLounge, "lounge", "Lounge", false, {0.12f, 0.10f, 0.09f}, {0.30f, 0.24f, 0.20f},
     {0.25f, 0.18f, 0.13f}, true, 0.3f, 3.5f, 2.6f, 0.f},
    {EnvironmentId::kSpace, "space", "Space", false, {0.f, 0.f, 0.02f}, {0.03f, 0.02f, 0.08f}, {0.f, 0.f, 0.f}, false,
     0.2f, 6.f, 7.f, 0.8f},
    {EnvironmentId::kPassthrough, "passthrough", "Passthrough", true, {0.f, 0.f, 0.f}, {0.f, 0.f, 0.f},
     {0.f, 0.f, 0.f}, false, 0.f, 2.5f, 2.2f, 0.f},
}};

inline const EnvironmentSpec& EnvironmentByWireName(const std::string& name) {
  for (const auto& e : kEnvironments) {
    if (name == e.wire_name) return e;
  }
  return kEnvironments[static_cast<size_t>(EnvironmentId::kCinema)];
}

}  // namespace xr

#endif  // PLEZY_LINUX_XR_ENVIRONMENT_H_
