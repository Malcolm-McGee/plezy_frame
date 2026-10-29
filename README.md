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
~/plezy/plezy-steamos.sh
```

To launch it from the Steam library, choose *Add a Non-Steam Game* and pick `~/plezy/plezy-steamos.sh`.

`plezy-steamos.sh` starts Plezy as a **VR program** (see *Using it* below). Use it rather than `plezy.sh`, including in Game Mode: Plezy's video needs a Wayland session with sub-surface support, which the desktop has but Game Mode's compositor (gamescope) does not, so the launcher runs Plezy inside a small invisible private session (a virtual KWin). Nothing is drawn on the desktop or in Game Mode; the headset is the interface.

If something fails, the script stops and prints the step it was on. Run it again after fixing the cause; finished steps are quick the second time.

## Using it

Start **Plezy VR** (from Steam, or `~/plezy/plezy-steamos.sh`). You go straight into VR: your chosen environment with your library on a floating screen. No window opens on the desktop or in Game Mode.

- **Browse:** Continue Watching and each of your libraries (movies, shows, a "3D Movies" library, and so on), as poster grids. Shows open into seasons and episodes. Point with either controller and pull the trigger to pick; the stick scrolls; the back button or the menu button goes back.
- **Watch:** the title plays on a big curved screen in the environment, in 2D or 3D. The menu button (left) stops and returns to the menu. The last tile on the home screen is *Exit Plezy VR*.

| Control | In the menu | While watching |
| --- | --- | --- |
| Trigger | Pick what you are pointing at | Play / pause |
| Stick up / down | Scroll | Volume |
| Stick left / right | | Seek 10 seconds |
| `B` | Next environment | Next environment |
| `A` | | Next 3D mode (flat, side-by-side half/full, top-bottom half/full) |
| Left stick click | Recenter | Recenter |
| Menu button (left) | Back | Stop and return to the menu |

Bindings are suggested for the Khronos simple, Oculus Touch and Valve Index profiles and can be rebound in SteamVR.

**Environments:** Void, Cinema, Lounge, Space and Passthrough (the screen floats over your real room; needs a runtime that offers alpha-blend or additive passthrough). The default is set under *Settings → Video playback → VR cinema*.

**3D movies:** side-by-side and top-bottom, half and full width. The format is read from the file's metadata, then from tags in the file name (`HSBS`, `FSBS`, `H-OU`, `FTAB`, ...). A title that just says "3D", or sits in a library whose name contains "3D", is shown as side-by-side; if that is wrong, press `A` to cycle through the modes.

### First-time setup

Plezy VR uses the account you signed in with, so sign in to your Plex, Jellyfin or Emby server once with the normal window:

```
PLEZY_VR=0 ~/plezy/plezy-steamos.sh
```

(`PLEZY_VR=0` runs Plezy as an ordinary desktop app.) After that, Plezy VR just works.

## Performance

The headset runs at its full refresh rate (72 Hz on a Steam Frame) while video plays. To keep it that way:

- Video is decoded by the Frame's **hardware decoder** where it can: H.264 and 8-bit HEVC at any size. Its 10-bit HEVC path crashes the chip's firmware, so 10-bit sources are decoded in **software**, which keeps up to about 1080p.
- A film the headset cannot decode (10-bit HEVC above 1080p, such as a 4K HDR rip) is **converted by your media server** to 1080p H.264, which the hardware decoder then plays. Side-by-side 3D survives the conversion. This needs a server that can transcode 4K in real time; if yours cannot, the picture stalls and the progress bar turns amber while it waits. A 1080p copy of the film avoids the problem.
- The picture is drawn at 1.25x the headset's recommended resolution with 4x multisampling and trilinear filtering. `PLEZY_XR_SCALE` and `PLEZY_XR_MSAA` change this if your headset needs a lighter load.

## If something goes wrong

Each launch keeps its output in two files:

- `~/.local/state/plezy-launch.log`: the app (sign-in, server, library, which decoder was chosen and why).
- `~/.local/state/plezy-xr.log`: the headset side (frame rate, decoder, errors from the video player).

`~/plezy/plezy-xr --menu` runs the headset side alone, and prints why it could not start (no runtime, a missing OpenGL ES extension) if it fails.

## How it works

Plezy VR is two programs. `plezy-xr` (`linux/xr/`) is the headset side: it opens the OpenXR session, draws the environment, the menu and the cinema screen, plays video with libmpv, and reads the controllers. Plezy itself runs invisibly in a virtual compositor and does everything that needs the server: sign-in, browsing, resume points, watched state, and picking how to play each title. They talk over a line protocol on the helper's stdin and stdout (`linux/xr/xr_protocol.h`). It needs an OpenXR runtime that exposes `XR_KHR_opengl_es_enable` and `XR_MNDX_egl_enable` (both Monado and SteamVR on the Steam Frame do).

Testing without a headset: `plezy-xr --dump-ui menu.png` writes a demo menu to a PNG; `PLEZY_XR_DUMP_UI=file.png` does the same for the live menu; `PLEZY_VR_AUTOPICK="Anime;#first;#stop:20"` drives the menu with no controller; `PLEZY_XR_NO_AUTOPAUSE=1` keeps playing when the headset is not worn; `PLEZY_XR_PROFILE=1` prints per-frame render timings.

## Status

Tested on a Steam Frame with SteamVR: the headset session, the library menu with real posters, pointing and selecting, browsing show → season → episode, playback in 2D and 3D, all five environments, and the decoder choices above. Top-bottom 3D and the Passthrough environment have been tried with test clips only.

## License and credits

Licensed under [GPL-3.0](LICENSE). This is a fork of [Plezy](https://github.com/edde746/plezy) by edde746, built with [Flutter](https://flutter.dev) and [mpv](https://mpv.io). The original project's README is at the upstream link above.
