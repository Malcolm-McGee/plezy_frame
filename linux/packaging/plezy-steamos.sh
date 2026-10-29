#!/usr/bin/env bash
# Launcher for Plezy on SteamOS / Steam Frame.
#
# Plezy's video plane is a Wayland sub-surface, so it needs a compositor that
# offers wl_subcompositor. The Plasma desktop session does; Game Mode (gamescope)
# does not, and only gives games an X11 display. When there is no usable Wayland
# session this starts a small private one (a nested KWin, drawn inside the X11
# window gamescope provides) and runs Plezy inside it.
#
# By default it starts Plezy as a VR program: the headset menu (library, shows,
# posters, the cinema screen) is the app. The Plezy window still exists, because it
# is the engine that talks to the media server, but it lives in an invisible virtual
# compositor and nothing is shown on the desktop or in Game Mode. Set PLEZY_VR=0 to
# run it as an ordinary desktop app instead (needed once, to sign in to a server).
#
# Set PLEZY_FORCE_NESTED=1 to always use the nested session, or
# PLEZY_NESTED_SIZE=WIDTHxHEIGHT to override the detected screen size.
set -u

export PLEZY_VR="${PLEZY_VR-1}"

DIR="$(dirname "$(readlink -f "$0")")"

# Game Mode hides a launch's output, so keep the latest one where it can be read:
# ~/.local/state/plezy-launch.log
log_file="${XDG_STATE_HOME:-$HOME/.local/state}/plezy-launch.log"
mkdir -p "$(dirname "$log_file")" 2>/dev/null
exec > >(tee "$log_file") 2>&1
echo "plezy-steamos: $(date) DISPLAY=${DISPLAY:-} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-} XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-}"

on_usable_wayland() {
  [[ -z "${PLEZY_FORCE_NESTED:-}" ]] || return 1
  [[ -n "${WAYLAND_DISPLAY:-}" ]] || return 1
  # gamescope's own Wayland socket has no sub-surface support.
  [[ "$WAYLAND_DISPLAY" != "${GAMESCOPE_WAYLAND_DISPLAY:-gamescope-0}" ]] || return 1
}

if [[ "$PLEZY_VR" != "1" ]] && on_usable_wayland; then
  export GDK_BACKEND=wayland
  exec "$DIR/plezy.sh" "$@"
fi

if [[ "$PLEZY_VR" != "1" && -z "${DISPLAY:-}" ]]; then
  echo "plezy: no Wayland or X11 display to run in." >&2
  exit 1
fi
if ! command -v kwin_wayland >/dev/null 2>&1; then
  echo "plezy: needs kwin_wayland to run outside a Wayland desktop, and it was not found." >&2
  exit 1
fi

size="${PLEZY_NESTED_SIZE:-}"
if [[ "$PLEZY_VR" == "1" ]]; then
  size="${size:-1920x1080}"  # invisible, so the size only has to be sensible
elif [[ -z "$size" ]] && command -v xdpyinfo >/dev/null 2>&1; then
  size="$(xdpyinfo 2>/dev/null | awk '/dimensions:/ {print $2; exit}')"
fi
size="${size:-1920x1080}"
width="${size%x*}"
height="${size#*x}"

# A KWin rule that makes the Plezy window fill the nested screen. KWin reads it
# from a private config folder; the app gets the user's real one back.
rules_home="${XDG_RUNTIME_DIR:-/tmp}/plezy-kwin-$$"
mkdir -p "$rules_home"
cat > "$rules_home/kwinrulesrc" <<'EOF'
[General]
count=1
rules=1

[1]
Description=Plezy fills the screen
clientmachine=localhost
fullscreen=true
fullscreenrule=2
noborder=true
noborderrule=2
wmclass=com.edde746.plezy
wmclasscomplete=false
wmclassmatch=1
EOF
trap 'rm -rf "$rules_home"' EXIT

# The nested session renders through X11/GL, so gamescope's Vulkan layer has
# nothing to hook and only raises "power through this error" warnings.
export ENABLE_GAMESCOPE_WSI=0
export GDK_BACKEND=wayland
export PLEZY_ORIGINAL_XDG_CONFIG_HOME="${XDG_CONFIG_HOME-}"

# Session command: give the app the user's real config folder back.
session="$rules_home/session.sh"
cat > "$session" <<EOF
#!/usr/bin/env bash
if [[ -n "\${PLEZY_ORIGINAL_XDG_CONFIG_HOME:-}" ]]; then
  export XDG_CONFIG_HOME="\$PLEZY_ORIGINAL_XDG_CONFIG_HOME"
else
  unset XDG_CONFIG_HOME
fi
exec "$DIR/plezy.sh" "\$@"
EOF
chmod +x "$session"

if [[ "$PLEZY_VR" == "1" ]]; then
  display_args=(--virtual)  # no window anywhere: the compositor renders to memory
else
  display_args=(--x11-display "$DISPLAY")
fi

XDG_CONFIG_HOME="$rules_home" kwin_wayland \
  "${display_args[@]}" \
  --width "$width" --height "$height" \
  --socket "plezy-$$" \
  --no-lockscreen --no-global-shortcuts \
  --exit-with-session "$session"
