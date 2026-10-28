#ifndef PLEZY_LINUX_XR_STEREO_LAYOUT_H_
#define PLEZY_LINUX_XR_STEREO_LAYOUT_H_

#include <string>

// How a decoded video frame packs its two eyes, and the texture rect each eye
// samples. Header-only and free of OpenXR/GL so it can be tested on its own.
// The enum names are the wire names used by lib/media/stereo_3d.dart.

namespace xr {

enum class StereoLayout {
  kNone,
  kSideBySideHalf,
  kSideBySideFull,
  kTopBottomHalf,
  kTopBottomFull,
};

enum class Eye { kLeft = 0, kRight = 1 };

// A normalised sub-rectangle of the video texture, origin top-left.
struct UvRect {
  float u0, v0, u1, v1;
};

inline StereoLayout StereoLayoutFromWireName(const std::string& name) {
  if (name == "sideBySideHalf") return StereoLayout::kSideBySideHalf;
  if (name == "sideBySideFull") return StereoLayout::kSideBySideFull;
  if (name == "topBottomHalf") return StereoLayout::kTopBottomHalf;
  if (name == "topBottomFull") return StereoLayout::kTopBottomFull;
  return StereoLayout::kNone;
}

// The rect of the frame that [eye] should see. 2D video shows the whole frame
// to both eyes. [swap_eyes] is for right-eye-first packings.
inline UvRect EyeUvRect(StereoLayout layout, Eye eye, bool swap_eyes) {
  const bool second = (eye == Eye::kRight) != swap_eyes;
  switch (layout) {
    case StereoLayout::kSideBySideHalf:
    case StereoLayout::kSideBySideFull:
      return second ? UvRect{0.5f, 0.f, 1.f, 1.f} : UvRect{0.f, 0.f, 0.5f, 1.f};
    case StereoLayout::kTopBottomHalf:
    case StereoLayout::kTopBottomFull:
      return second ? UvRect{0.f, 0.5f, 1.f, 1.f} : UvRect{0.f, 0.f, 1.f, 0.5f};
    case StereoLayout::kNone:
      break;
  }
  return UvRect{0.f, 0.f, 1.f, 1.f};
}

// Aspect ratio (width / height) of ONE eye's image as it should appear on the
// screen. Half-packed layouts are squeezed on one axis and must be stretched
// back, so the displayed ratio is the packed frame's ratio un-squeezed.
inline float EyeDisplayAspect(StereoLayout layout, int frame_width, int frame_height) {
  if (frame_width <= 0 || frame_height <= 0) return 16.f / 9.f;
  const float ratio = static_cast<float>(frame_width) / static_cast<float>(frame_height);
  switch (layout) {
    case StereoLayout::kSideBySideHalf:
      return ratio;  // each eye is w/2 x h, stored squeezed: true aspect is w/h
    case StereoLayout::kSideBySideFull:
      return ratio / 2.f;  // each eye is w/2 x h at native shape
    case StereoLayout::kTopBottomHalf:
      return ratio;  // each eye is w x h/2, stored squeezed: true aspect is w/h
    case StereoLayout::kTopBottomFull:
      return ratio * 2.f;  // each eye is w x h/2 at native shape
    case StereoLayout::kNone:
      break;
  }
  return ratio;
}

}  // namespace xr

#endif  // PLEZY_LINUX_XR_STEREO_LAYOUT_H_
