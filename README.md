# Plezy for Steam Frame

A VR cinema for [Plezy](https://github.com/edde746/plezy), the Plex, Jellyfin and Emby client. Watch your library on a huge curved screen in a virtual environment on a Steam Frame (or any OpenXR headset), including 3D movies.

- **Environments:** Void, Cinema, Lounge, Space, and Passthrough (the screen floats over your real room; needs a runtime that offers alpha-blend or additive passthrough).
- **3D movies:** side-by-side and top-bottom, half and full width. The format is read from the file's metadata, then from tags in the file name (`HSBS`, `FSBS`, `H-OU`, `FTAB`, ...).

## Install on a Steam Frame

There is no prebuilt download yet, so you build it on the Frame itself. It needs no root access and does not change SteamOS: everything lands in your home folder. Allow roughly half an hour the first time.

Before you start: switch to **Desktop Mode** and make sure **SteamVR** is installed (it provides the OpenXR runtime). Open a terminal (Konsole) and run these three commands, one at a time.

**1. Get Plezy**
```
git clone -b steam-frame-xr-foundation https://github.com/Malcolm-McGee/plezy_frame
```

**2. Get Flutter** (the toolkit Plezy is built with)
```
git clone --depth 1 -b stable https://github.com/flutter/flutter.git ~/flutter
```

**3. Build and install**
```
cd plezy_frame && ./scripts/steamos_build.sh
```

The script downloads what SteamOS lacks (libmpv, two small libraries it needs, and a JDK that one Flutter package requires to configure the build), builds the app and the VR helper, and installs them to `~/plezy`. The first Flutter run also downloads Flutter's own tools and git history, which takes several minutes; let it finish.

**Start it**
```
~/plezy/plezy.sh
```

To launch it from the Steam library, choose *Add a Non-Steam Game* and pick `~/plezy/plezy.sh`.

If something fails, the script stops and prints the step it was on. Run it again after fixing the cause; finished steps are quick the second time.

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

The VR player is a separate program, `plezy-xr` (`linux/xr/`), started by the app when you press the button. It is built only when the OpenXR development files are installed, so without them the button stays hidden. It needs an OpenXR runtime that exposes `XR_KHR_opengl_es_enable` and `XR_MNDX_egl_enable` (both Monado and SteamVR on the Steam Frame do; on other runtimes, read the helper's error message if VR fails to start). You can run it by hand with `plezy-xr --url <stream> [--env cinema] [--layout sideBySideHalf]`, and set `PLEZY_XR_BIN` to point the app at a helper outside the bundle.

## Status

Tested on a Steam Frame with SteamVR: the VR helper starts a session, plays video, shows all five environments, and splits side-by-side 3D into the correct eye. The player's **Watch in VR cinema** button, the settings screen, and top-bottom 3D have been written and unit-tested but not yet tried on the headset. Please report what the helper prints when it fails: run `~/plezy/plezy-xr --url <video file or URL>` in a terminal.

## License and credits

Licensed under [GPL-3.0](LICENSE). This is a fork of [Plezy](https://github.com/edde746/plezy) by edde746, built with [Flutter](https://flutter.dev) and [mpv](https://mpv.io). The original project's README is at the upstream link above.
