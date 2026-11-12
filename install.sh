#!/usr/bin/env bash
# shimmy — streamlined installer.
# Bootstraps the Arch Linux (aarch64) rootfs, installs the desktop (Hyprland or KDE)
# per the Arch wiki, and installs the rdmetal driver so the desktop "just works".
#
# Usage:
#   ./install.sh                    # default desktop = hyprland
#   ./install.sh --desktop kde      # KDE Plasma instead
#
# Produces a rootfs/ directory (NOT stored in git) and a built shim.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DESKTOP="${1:-hyprland}"
case "${2:-$DESKTOP}" in --desktop) DESKTOP="$3";; esac
ROOTFS="$HERE/rootfs"
ARCH_TARBALL="ArchLinuxARM-aarch64-latest.tar.gz"
ARCH_URL="http://os.archlinuxarm.org/os/$ARCH_TARBALL"

# --- 0. prerequisites ---------------------------------------------------------
command -v curl >/dev/null || { echo "need curl"; exit 1; }
command -v clang >/dev/null || { echo "need clang"; exit 1; }

# --- 1. build the shim --------------------------------------------------------
echo "==> building the shim"
(cd "$HERE" && ./build.sh gui && ./build.sh cli)

# --- 2. bootstrap the Arch rootfs (not stored; built here) --------------------
if [ ! -x "$ROOTFS/usr/bin/pacman" ]; then
  echo "==> downloading Arch Linux ARM aarch64 base rootfs (~830 MB)"
  [ -f "/tmp/$ARCH_TARBALL" ] || curl -sL "$ARCH_URL" -o "/tmp/$ARCH_TARBALL"
  mkdir -p "$ROOTFS"
  echo "==> extracting rootfs"
  tar -xzf "/tmp/$ARCH_TARBALL" -C "$ROOTFS" 2>/dev/null || true
fi

# --- 3. make pacman work under the shim --------------------------------------
echo "==> configuring the guest (DNS, mount table, pacman)"
# DNS: resolv.conf is a broken symlink in the tarball
rm -f "$ROOTFS/etc/resolv.conf"
printf 'nameserver 1.1.1.1\nnameserver 8.8.8.8\n' > "$ROOTFS/etc/resolv.conf"
# pacman needs a mount table + no GPG signature check (keyring can't be written)
[ -e "$ROOTFS/etc/mtab" ] || printf '/dev/root / ext4 rw,relatime 0 0\nproc /proc proc rw 0 0\n' > "$ROOTFS/etc/mtab"
sed -i '' 's/^#*SigLevel.*/SigLevel = Never/' "$ROOTFS/etc/pacman.conf"
# /dev/shm + /dev/dri placeholders (keymap shm_open + the DRM render node)
mkdir -p "$ROOTFS/dev/shm" "$ROOTFS/dev/dri"

SHIM_CLI="$HERE/shim_cli"
run_pacman() { RDUID0=1 "$SHIM_CLI" "$ROOTFS" /usr/bin/pacman "$@" --disable-sandbox; }

# --- 4. sync + install the desktop per the Arch wiki --------------------------
echo "==> pacman -Sy"
run_pacman -Sy --noconfirm
case "$DESKTOP" in
  kde|plasma)
    echo "==> installing KDE Plasma (standard Arch install)"
    run_pacman -S --noconfirm plasma-meta konsole dolphin sddm
    ;;
  *)
    echo "==> installing Hyprland (standard Arch install)"
    run_pacman -S --noconfirm hyprland foot gtk3 gtk4 noto-fonts
    ;;
esac
# common desktop apps
run_pacman -S --noconfirm mesa mesa-utils libglvnd wayland wayland-protocols \
  fastfetch btop htop chromium mpv 2>/dev/null || true

# --- 5. install the rdmetal driver (so the desktop renders) -------------------
echo "==> installing the rdmetal driver"
if [ -d "$HERE/rdmetal-libs" ]; then
  install -d "$ROOTFS/usr/lib/dri"
  install -m 644 "$HERE/rdmetal-libs/dri/rdmetal_dri.so" "$ROOTFS/usr/lib/dri/rdmetal_dri.so"
  for lib in libEGL.so.1.0.0 libGLESv2.so.2.0.0 libgbm.so.1.0.0 libglapi.so.0.0.0; do
    [ -f "$HERE/rdmetal-libs/$lib" ] && install -m 644 "$HERE/rdmetal-libs/$lib" "$ROOTFS/usr/lib/$lib"
  done
  # repoint the versioned symlinks to our libs
  ln -sf libEGL.so.1.0.0   "$ROOTFS/usr/lib/libEGL.so.1"
  ln -sf libGLESv2.so.2.0.0 "$ROOTFS/usr/lib/libGLESv2.so.2"
  ln -sf libgbm.so.1.0.0   "$ROOTFS/usr/lib/libgbm.so.1"
  ln -sf libglapi.so.0.0.0 "$ROOTFS/usr/lib/libglapi.so.0"
  echo "    -> rdmetal_dri.so + EGL/GLES/gbm libs installed; default driver (no env vars)"
else
  echo "    ! rdmetal-libs/ not found — run the rdmetal build first (see shimmy-rdmetal)"
fi

echo ""
echo "==> done."
echo "    run:  ./run.sh --desktop $DESKTOP"
