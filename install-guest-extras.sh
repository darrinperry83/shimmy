#!/bin/bash
# install-guest-extras.sh — add optional guest packages/config to an existing Arch rootfs (idempotent).
#
#   ./install-guest-extras.sh                      # --rootfs arch (bare base: audio stack only)
#   ./install-guest-extras.sh --rootfs omarchy     # Arch + Hyprland + Quickshell + Omarchy config + app set
#   ROOTFS=/path ./install-guest-extras.sh --rootfs omarchy   # override the target rootfs
#
# `--rootfs arch` (default) installs the PipeWire/WirePlumber audio stack only (the bare
# base). `--rootfs omarchy` bakes in the Omarchy desktop experience on top of that base:
#
#   * pacman -S hyprland foot noto-fonts   (window manager + a Wayland client + fonts)
#   * a glibc Quickshell 0.3.2 + its QML modules (built by ../quickshell-arch; not in pacman)
#   * the omarchy-config tree (defaults -> /usr/share/omarchy, personal config -> ~/.config)
#   * the runnable Omarchy-styled bar -> ~/.config/quickshell (shell.qml + qml/)
#   * /usr/share/omarchy/omarchy.lua (consolidated launch config; autostarts the bar)
#
# The desktop itself is added/changed by the user via pacman; there is no --desktop flag.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="${ROOTFS:-$HERE/rootfs}"
SHIM="$HERE/shim_cli"
QS_PAYLOAD="$HERE/../quickshell-arch/payload"
OMARCHY="$HERE/../omarchy-config"
OMARCHY_BAR="$HERE/../omarchy-bar"
ROOTFS_TYPE="arch"

while [ $# -gt 0 ]; do
  case "$1" in
    --rootfs) ROOTFS_TYPE="${2:-arch}"; shift 2 ;;
    -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1 (use --rootfs arch|omarchy)" >&2; exit 2 ;;
  esac
done
case "$ROOTFS_TYPE" in arch|omarchy) ;; *) echo "unknown --rootfs: $ROOTFS_TYPE (arch|omarchy)" >&2; exit 2 ;; esac

[ -d "$ROOT" ] || { echo "error: rootfs not found at $ROOT (set ROOTFS=...)" >&2; exit 1; }
[ -x "$SHIM" ] || { echo "error: $SHIM missing — run ./build.sh cli first" >&2; exit 1; }

echo "== rdmetal install (--rootfs $ROOTFS_TYPE) =="
echo "rootfs: $ROOT"

# ---- audio stack (both variants) ---------------------------------------------------------
if [ -x "$ROOT/usr/bin/pipewire" ] && [ -x "$ROOT/usr/bin/wireplumber" ] && \
   [ -x "$ROOT/usr/bin/pipewire-pulse" ]; then
  echo "audio packages already present (pipewire wireplumber pipewire-pulse)"
else
  echo "== pacman: sync + install pipewire wireplumber pipewire-pulse =="
  RDUID0=1 "$SHIM" "$ROOT" /usr/bin/pacman -Sy --noconfirm --disable-sandbox
  RDUID0=1 "$SHIM" "$ROOT" /usr/bin/pacman -S --noconfirm --disable-sandbox \
    pipewire wireplumber pipewire-pulse
fi

mkdir -p "$ROOT/usr/local/bin"
install -m 0755 "$HERE/audio-boot.sh" "$ROOT/usr/local/bin/rdmetal-audio-boot.sh"
echo "installed $ROOT/usr/local/bin/rdmetal-audio-boot.sh"

# ---- synthetic input sysfs surface (evdev.c) ---------------------------------------------
# libinput's evdev_device_have_same_syspath() compares the syspath of the device it
# enumerates via /sys/class/input/eventN against the syspath libudev's
# sd_device_new_from_devnum() derives by chasing /sys/dev/char/13:6N. On real Linux the
# /sys/dev/char entry is a SYMLINK to the device dir, so chase() resolves both to the same
# canonical path. Mirror that here: eventN are real placeholder dirs (whose uevent/dev/
# subsystem FILE bodies evdev.c serves synthetically), and 13:6N are symlinks back to them.
mkdir -p "$ROOT/sys/class/input/event0" "$ROOT/sys/class/input/event1"
ln -sfn /sys/class/input/event0 "$ROOT/sys/dev/char/13:64"
ln -sfn /sys/class/input/event1 "$ROOT/sys/dev/char/13:65"
echo "created $ROOT/sys/class/input/event{0,1} + $ROOT/sys/dev/char/13:{64,65} symlinks"

# ---- Omarchy experience --------------------------------------------------------------
install_omarchy() {
  # 1. Window manager + client set via pacman (idempotent).
  if [ -x "$ROOT/usr/bin/Hyprland" ] && [ -x "$ROOT/usr/bin/foot" ]; then
    echo "hyprland + foot already present"
  else
    echo "== pacman: install hyprland foot noto-fonts =="
    RDUID0=1 "$SHIM" "$ROOT" /usr/bin/pacman -Sy --noconfirm --disable-sandbox
    RDUID0=1 "$SHIM" "$ROOT" /usr/bin/pacman -S --noconfirm --disable-sandbox \
      hyprland foot noto-fonts
  fi

  # 2. glibc Quickshell + QML modules (built separately; not in pacman).
  if [ -x "$ROOT/usr/bin/quickshell" ]; then
    echo "quickshell already present in $ROOT/usr/bin"
  elif [ -d "$QS_PAYLOAD/usr" ]; then
    echo "== install quickshell (glibc) from $QS_PAYLOAD =="
    cp -R "$QS_PAYLOAD/usr/." "$ROOT/usr/"
  else
    echo "error: quickshell not installed and $QS_PAYLOAD missing — build it first" >&2
    echo "       (see ../quickshell-arch/configure.sh + cmake --build)" >&2
    exit 1
  fi

  # 3. Omarchy config tree.
  echo "== install omarchy config =="
  mkdir -p "$ROOT/usr/share/omarchy/default" "$ROOT/usr/share/omarchy/shell"
  cp -R "$OMARCHY/default/." "$ROOT/usr/share/omarchy/default/"
  cp "$OMARCHY/shell/shell.qml" "$ROOT/usr/share/omarchy/shell/shell.qml" 2>/dev/null || true
  install -m 0644 "$HERE/../omarchy.lua" "$ROOT/usr/share/omarchy/omarchy.lua"

  mkdir -p "$ROOT/root/.config/hypr" "$ROOT/root/.config/quickshell"
  cp -R "$OMARCHY/config/hypr/." "$ROOT/root/.config/hypr/"
  cp "$OMARCHY_BAR/shell.qml" "$ROOT/root/.config/quickshell/shell.qml"
  cp -R "$OMARCHY_BAR/qml/." "$ROOT/root/.config/quickshell/qml/"
  echo "installed /usr/share/omarchy (defaults + omarchy.lua), ~/.config/hypr, ~/.config/quickshell"
}

[ "$ROOTFS_TYPE" = omarchy ] && install_omarchy

echo "== install done. next: ./run.sh --rootfs $ROOTFS_TYPE --gui =="
