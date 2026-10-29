/// Frame-packed stereoscopic (3D) layouts Plezy can split into per-eye images.
enum Stereo3dLayout {
  /// Ordinary 2D video.
  none,

  /// Left eye in the left half, right eye in the right half, each squeezed to
  /// half width ("HSBS").
  sideBySideHalf,

  /// Side by side with both eyes at full width ("FSBS"); the frame is twice the
  /// width of a single eye.
  sideBySideFull,

  /// Left eye on top, right eye below, each squeezed to half height ("HOU").
  topBottomHalf,

  /// Top/bottom with both eyes at full height.
  topBottomFull;

  bool get isStereo => this != none;

  bool get isSideBySide => this == sideBySideHalf || this == sideBySideFull;

  bool get isTopBottom => this == topBottomHalf || this == topBottomFull;

  /// Value shared with the native renderer's `StereoLayout` enum.
  String get wireName => name;

  static Stereo3dLayout fromWireName(String? value) =>
      Stereo3dLayout.values.firstWhere((l) => l.name == value, orElse: () => none);
}

/// Guesses a [Stereo3dLayout] from the file name or title.
///
/// Scene-style names carry the packing as a delimited token: `Movie.2012.3D.HSBS.mkv`,
/// `Movie (2012) [H-OU]`. Tokens are matched whole, so a title that merely
/// contains the letters ("Absbsb") never triggers. Returns [Stereo3dLayout.none]
/// when nothing reliable is found; a bare `3D` tag does not say how the eyes are
/// packed, so it is not enough on its own.
Stereo3dLayout detectStereo3dFromName(String? name, {bool guessFromBare3dTag = false}) {
  if (name == null || name.isEmpty) return Stereo3dLayout.none;
  final tokens = name.toLowerCase().split(RegExp(r'[^a-z0-9]+')).where((t) => t.isNotEmpty).toList();
  // "h-sbs", "half-ou" arrive as separate tokens; look at adjacent pairs too.
  final candidates = <String>{...tokens, for (var i = 0; i + 1 < tokens.length; i++) '${tokens[i]}${tokens[i + 1]}'};
  const halfSbs = {'hsbs', 'halfsbs', 'sbs', 'sidebyside', 'halfsidebyside', 'hsidebyside'};
  const fullSbs = {'fsbs', 'fullsbs', 'fullsidebyside'};
  const halfTab = {'htab', 'hou', 'halfou', 'halftab', 'tab', 'ou', 'overunder', 'topbottom', 'halfoverunder'};
  const fullTab = {'ftab', 'fou', 'fullou', 'fulltab', 'fulloverunder'};
  // Full variants first: "full" + "sbs" must not be read as the half default.
  if (candidates.any(fullSbs.contains)) return Stereo3dLayout.sideBySideFull;
  if (candidates.any(fullTab.contains)) return Stereo3dLayout.topBottomFull;
  if (candidates.any(halfSbs.contains)) return Stereo3dLayout.sideBySideHalf;
  if (candidates.any(halfTab.contains)) return Stereo3dLayout.topBottomHalf;
  // A lone "3D" says it is 3D but not how the eyes are packed. Half side-by-side is by far the most
  // common, and it can be changed in VR, so it is a better guess than showing two pictures.
  if (guessFromBare3dTag && candidates.contains('3d')) return Stereo3dLayout.sideBySideHalf;
  return Stereo3dLayout.none;
}

/// Maps a Matroska `StereoMode` / ffmpeg `stereo3d` value to a layout.
///
/// Accepts the numeric element value (2 = top/bottom, 1 = side by side, ...) or
/// the ffmpeg names. Only left-eye-first packings are mapped; right-eye-first
/// variants are reported through [Stereo3dInfo.swapEyes].
Stereo3dInfo stereo3dFromContainer(Object? stereoMode, {int? frameWidth, int? frameHeight}) {
  final raw = stereoMode?.toString().toLowerCase().trim();
  if (raw == null || raw.isEmpty) {
    return const Stereo3dInfo(Stereo3dLayout.none);
  }
  final ratio = (frameWidth != null && frameHeight != null && frameHeight > 0) ? frameWidth / frameHeight : null;
  // A full-width side-by-side frame is ~32:9 for 16:9 eyes; half is ~16:9 itself.
  final wide = ratio != null && ratio > 3.0;
  final tall = ratio != null && ratio < 1.0;
  switch (raw) {
    case '1': // left-right
    case 'side by side':
    case 'side_by_side':
    case 'left_right':
      return Stereo3dInfo(wide ? Stereo3dLayout.sideBySideFull : Stereo3dLayout.sideBySideHalf);
    case '11': // right-left
    case 'side_by_side_quincunx':
    case 'right_left':
      return Stereo3dInfo(wide ? Stereo3dLayout.sideBySideFull : Stereo3dLayout.sideBySideHalf, swapEyes: true);
    case '2': // bottom-top
    case 'bottom_top':
      return Stereo3dInfo(tall ? Stereo3dLayout.topBottomFull : Stereo3dLayout.topBottomHalf, swapEyes: true);
    case '3': // top-bottom
    case 'top bottom':
    case 'top_bottom':
    case 'top-bottom':
      return Stereo3dInfo(tall ? Stereo3dLayout.topBottomFull : Stereo3dLayout.topBottomHalf);
  }
  return const Stereo3dInfo(Stereo3dLayout.none);
}

/// Maps mpv's `video-params/stereo-in` (derived by ffmpeg from container
/// metadata) to a layout. mpv does not tell half from full width, so the frame
/// shape decides, as in [stereo3dFromContainer].
Stereo3dInfo stereo3dFromMpvStereoIn(String? value, {int? frameWidth, int? frameHeight}) {
  final raw = value?.toLowerCase().trim();
  if (raw == null || raw.isEmpty) return const Stereo3dInfo(Stereo3dLayout.none);
  final ratio = (frameWidth != null && frameHeight != null && frameHeight > 0) ? frameWidth / frameHeight : null;
  final wide = ratio != null && ratio > 3.0;
  final tall = ratio != null && ratio < 1.0;
  return switch (raw) {
    'sbs2l' => Stereo3dInfo(wide ? Stereo3dLayout.sideBySideFull : Stereo3dLayout.sideBySideHalf),
    'sbs2r' => Stereo3dInfo(wide ? Stereo3dLayout.sideBySideFull : Stereo3dLayout.sideBySideHalf, swapEyes: true),
    'ab2l' => Stereo3dInfo(tall ? Stereo3dLayout.topBottomFull : Stereo3dLayout.topBottomHalf),
    'ab2r' => Stereo3dInfo(tall ? Stereo3dLayout.topBottomFull : Stereo3dLayout.topBottomHalf, swapEyes: true),
    _ => const Stereo3dInfo(Stereo3dLayout.none),
  };
}

/// A detected or user-chosen stereo packing plus which eye comes first.
class Stereo3dInfo {
  final Stereo3dLayout layout;

  /// True when the right-eye image is stored first (cross-eyed packing).
  final bool swapEyes;

  const Stereo3dInfo(this.layout, {this.swapEyes = false});

  bool get isStereo => layout.isStereo;

  @override
  bool operator ==(Object other) => other is Stereo3dInfo && other.layout == layout && other.swapEyes == swapEyes;

  @override
  int get hashCode => Object.hash(layout, swapEyes);

  @override
  String toString() => 'Stereo3dInfo($layout, swapEyes: $swapEyes)';
}

/// Picks the effective layout: a user override wins, then container metadata,
/// then the file name.
Stereo3dInfo resolveStereo3d({
  Stereo3dInfo? userOverride,
  Stereo3dInfo? container,
  String? name,
  bool guessFromBare3dTag = false,
}) {
  if (userOverride != null) return userOverride;
  if (container != null && container.isStereo) return container;
  return Stereo3dInfo(detectStereo3dFromName(name, guessFromBare3dTag: guessFromBare3dTag));
}
