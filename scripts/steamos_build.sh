#!/usr/bin/env bash
# Build Plezy with the VR cinema on a Steam Frame (SteamOS, aarch64) - no root needed.
#
# Needs: Desktop Mode, SteamVR installed, and Flutter cloned to ~/flutter:
#   git clone --depth 1 -b stable https://github.com/flutter/flutter.git ~/flutter
#
# Result: a ready-to-run folder at ~/plezy (override with PLEZY_INSTALL_DIR).
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
ROOT="$PWD"
FLUTTER_DIR="${FLUTTER_DIR:-$HOME/flutter}"
INSTALL_DIR="${PLEZY_INSTALL_DIR:-$HOME/plezy}"

if [[ ! -x "$FLUTTER_DIR/bin/flutter" ]]; then
  echo "Flutter not found at $FLUTTER_DIR. Install it first:" >&2
  echo "  git clone --depth 1 -b stable https://github.com/flutter/flutter.git $FLUTTER_DIR" >&2
  exit 1
fi
if [[ "$(uname -m)" != "aarch64" ]]; then
  echo "warning: this script targets the Steam Frame (aarch64); this machine is $(uname -m)." >&2
fi
if [[ ! -e /opt/steamvr/bin/linuxarm64/libopenxr_loader.so ]] && ! pkg-config --exists openxr; then
  echo "warning: no OpenXR loader found; install SteamVR so the VR helper can be built." >&2
fi

export PATH="$FLUTTER_DIR/bin:$PATH"
export FLUTTER_SUPPRESS_ANALYTICS=true CI=true

echo "==> 1/4 libmpv, its libraries, and a JDK"
python3 scripts/steamos_deps.py --dest "$ROOT/libmpv-prefix"
export JAVA_HOME="$ROOT/jdk"   # the jni package needs jni.h to configure the build
LIBDIR="$ROOT/libmpv-prefix/lib/aarch64-linux-gnu"
export PKG_CONFIG_PATH="$ROOT/libmpv-prefix/lib/pkgconfig:$LIBDIR/pkgconfig"
# The linker needs libmpv's own dependencies (Lua, MuJS) on its search path.
export LD_LIBRARY_PATH="$LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

echo "==> 2/4 Flutter packages (the first Flutter run downloads its tools and can take several minutes)"
flutter pub get

echo "==> 3/4 Building"
flutter build linux --release

echo "==> 4/4 Bundling libraries into $INSTALL_DIR"
BUNDLE="$ROOT/build/linux/arm64/release/bundle"
test -x "$BUNDLE/plezy" || { echo "build did not produce $BUNDLE/plezy" >&2; exit 1; }
if [[ ! -x "$BUNDLE/plezy-xr" ]]; then
  echo "error: plezy-xr (the VR helper) was not built; check that OpenXR headers and SteamVR are present." >&2
  exit 1
fi
bash linux/packaging/bundle-libs.sh "$BUNDLE"
cp linux/packaging/plezy.sh "$BUNDLE/plezy.sh"
rm -rf "$INSTALL_DIR"
mkdir -p "$INSTALL_DIR"
cp -a "$BUNDLE/." "$INSTALL_DIR/"

echo
echo "Done. Start it with:  $INSTALL_DIR/plezy.sh"
echo "Add that file as a non-Steam game to launch it from the Steam library."
