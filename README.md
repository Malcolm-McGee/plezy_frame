# Plezy for Steam Frame

A VR cinema for [Plezy](https://github.com/edde746/plezy), the Plex, Jellyfin and Emby client. Watch your library on a huge curved screen in a virtual environment on a Steam Frame (or any OpenXR headset), including 3D movies.

- **Environments:** Void, Cinema, Lounge, Space, and Passthrough (the screen floats over your real room; needs a runtime that offers alpha-blend or additive passthrough).
- **3D movies:** side-by-side and top-bottom, half and full width. The format is read from the file's metadata, then from tags in the file name (`HSBS`, `FSBS`, `H-OU`, `FTAB`, ...).

## Install on a Steam Frame

There is no prebuilt download with the VR cinema yet: the release packages above do not include `plezy-xr`, so for now you build Plezy yourself. Nothing below has been run on a real Steam Frame; it is the same Linux arm64 build the project already ships, plus the VR helper.

1. **Get a shell with build tools on arm64 Linux.** Either the Frame in desktop mode, or any other arm64 Linux machine (then copy the finished folder across, step 5). SteamOS keeps its root filesystem read-only, so the simplest route is a container such as [Distrobox](https://distrobox.it) with Ubuntu 24.04 rather than changing the system.
2. **Install the build dependencies** (Debian/Ubuntu names, from the project's CI):

   ```bash
   sudo apt-get install -y clang cmake ninja-build pkg-config libgtk-3-dev liblzma-dev \
     libstdc++-12-dev libepoxy-dev libcurl4-openssl-dev libevdev-dev libwayland-dev libegl-dev \
     libopenxr-dev libgles-dev python3 git
   ```

   Install the [Flutter SDK](https://docs.flutter.dev/get-started/install/linux) (3.47.0 or newer) too. `libopenxr-dev` and `libgles-dev` are what make the build include the VR helper.
3. **Build:**

   ```bash
   git clone -b steam-frame-xr-foundation https://github.com/Malcolm-McGee/plezy_frame.git
   cd plezy_frame
   python3 scripts/fetch_linux_libmpv.py --dest libmpv-prefix
   export PKG_CONFIG_PATH="$PWD/libmpv-prefix/lib/pkgconfig:$PWD/libmpv-prefix/lib/aarch64-linux-gnu/pkgconfig"
   flutter pub get && scripts/codegen.sh
   flutter build linux --release
   ```

4. **Bundle the libraries** so the folder runs on its own (this also picks up the OpenXR loader):

   ```bash
   BUNDLE=build/linux/arm64/release/bundle
   ls "$BUNDLE/plezy" "$BUNDLE/plezy-xr"     # both must exist
   bash linux/packaging/bundle-libs.sh "$BUNDLE"
   cp linux/packaging/plezy.sh "$BUNDLE/plezy.sh"   # launcher that points at the bundled libraries
   ```

   If `plezy-xr` is missing, the OpenXR packages from step 2 were not found at configure time; install them and rebuild.
5. **Run it.** Copy the `bundle` folder wherever you like (for example `~/plezy`) and start `~/plezy/plezy.sh`. To make it appear in the Steam library, use *Add a Non-Steam Game* and pick that file.
6. **Make sure a headset runtime is active.** Start SteamVR (or another OpenXR runtime) with the Frame connected before pressing the VR button. If the button does nothing useful, run `~/plezy/plezy-xr --url <any video URL>` in a terminal: it prints why it could not start, such as no runtime or a missing OpenGL ES extension.

## Using it

Open a title and press **Watch in VR cinema** in the player controls. The headset opens a curved screen in the environment chosen under *Settings → Video playback → VR cinema*. The desktop window shows "Watching in VR" until you take the headset off or choose **Exit VR**, and normal playback resumes where you left off. Turn **Detect 3D movies** off in the same settings group to play everything flat.

| Control | Action |
| --- | --- |
| Trigger | Play / pause |
| Right stick left / right | Seek 10 seconds |
| Right stick up / down | Volume |
| `B` | Next environment |
| `A` | Next 3D mode (flat, side-by-side half/full, top-bottom half/full) |
| Left stick click | Recenter the screen |
| Left menu | Exit VR |

Bindings are suggested for the Khronos simple, Oculus Touch and Valve Index profiles and can be rebound in SteamVR.

## How it works

The VR player is a separate program, `plezy-xr` (`linux/xr/`), started by the app when you press the button. It is built only when the OpenXR development files are installed, so without them the button stays hidden. It needs an OpenXR runtime that exposes `XR_KHR_opengl_es_enable` and `XR_MNDX_egl_enable` (Monado does; SteamVR's Linux support for these varies, so read the helper's error message if VR fails to start). You can run it by hand with `plezy-xr --url <stream> [--env cinema] [--layout sideBySideHalf]`, and set `PLEZY_XR_BIN` to point the app at a helper outside the bundle.

## Status

The VR cinema has been built and unit-tested, but not yet run on a real Steam Frame. Expect rough edges, and please report what the helper prints when it fails.

## License and credits

Licensed under [GPL-3.0](LICENSE). This is a fork of [Plezy](https://github.com/edde746/plezy) by edde746, built with [Flutter](https://flutter.dev) and [mpv](https://mpv.io). The original project's README is at the upstream link above.
