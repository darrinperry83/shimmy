#!/usr/bin/env bash
# shimmy — streamlined installer.
# Bootstraps the Arch Linux (aarch64) rootfs per the selected rootfs profile,
# installs the desktop (Hyprland or KDE) where the profile calls for it, and
# installs the rdmetal driver so the desktop "just works".
#
# Usage:
#   ./install.sh                    # arch (bare base; boots to the TTY login)
#   ./install.sh --rootfs omarchy   # Arch + Hyprland + Quickshell, pre-configured
#   ./install.sh --rootfs kiosk     # minimal base for one locked app (planned)
#   ./install.sh --rootfs recovery  # minimal recovery rootfs (graphical installer)
#   ./install.sh --dry-run [--rootfs <p>]   # print the resolved package set (no build/install)
#   ./install.sh --desktop kde      # legacy: force a KDE Plasma desktop install
#
# Produces a rootfs/ directory (NOT stored in git) and a built shim.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DESKTOP="hyprland"
DESKTOP_EXPLICIT=0
ROOTFS_PROFILE="arch"
DRY_RUN=0
while [ $# -gt 0 ]; do
  case "$1" in
    --desktop)
      DESKTOP="${2:-hyprland}"; DESKTOP_EXPLICIT=1
      [ $# -ge 2 ] && shift 2 || shift ;;
    --rootfs)
      ROOTFS_PROFILE="${2:-arch}"
      [ $# -ge 2 ] && shift 2 || shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    *) DESKTOP="$1"; DESKTOP_EXPLICIT=1; shift ;;
  esac
done
ROOTFS="$HERE/rootfs"
ARCH_TARBALL="ArchLinuxARM-aarch64-latest.tar.gz"
ARCH_URL="http://os.archlinuxarm.org/os/$ARCH_TARBALL"

# The recovery profile package set: a minimal rootfs that boots straight to the
# full-screen graphical installer (Qt6) with WiFi (wpa_supplicant + dhcpcd),
# the reinstall + disassembly tools (binutils/gdb/python), and the hardware
# bridges (bluez/bluez-utils -> host CoreBluetooth; usbutils -> host IOKit).
RECOVERY_PKGS="busybox coreutils wpa_supplicant dhcpcd binutils gdb python \
qt6-base qt6-wayland pyside6 cage bluez bluez-utils usbutils"

# --- 0. prerequisites ---------------------------------------------------------
command -v curl >/dev/null || { echo "need curl"; exit 1; }
command -v clang >/dev/null || { echo "need clang"; exit 1; }

# --- 1. resolve the profile's package set (pure; no side effects) -------------
INSTALL_DESKTOP=0
case "$ROOTFS_PROFILE" in
  omarchy) INSTALL_DESKTOP=1 ;;
  recovery|kiosk|arch) ;;
  *) echo "    ! unknown rootfs profile '$ROOTFS_PROFILE' (arch|omarchy|kiosk|recovery); assuming bare arch" ;;
esac
[ "$DESKTOP_EXPLICIT" = "1" ] && INSTALL_DESKTOP=1

DESKTOP_PKGS=""
EXTRA_PKGS=""
if [ "$INSTALL_DESKTOP" = "1" ]; then
  case "$DESKTOP" in
    kde|plasma) DESKTOP_PKGS="plasma-meta konsole dolphin sddm" ;;
    *)
      if [ "$ROOTFS_PROFILE" = "omarchy" ]; then
        DESKTOP_PKGS="hyprland quickshell foot gtk3 gtk4 noto-fonts"
      else
        DESKTOP_PKGS="hyprland foot gtk3 gtk4 noto-fonts"
      fi
      ;;
  esac
  EXTRA_PKGS="mesa mesa-utils libglvnd wayland wayland-protocols fastfetch btop htop chromium mpv"
fi

# --- 2. dry-run: report the resolved plan and stop ----------------------------
if [ "$DRY_RUN" = "1" ]; then
  echo "==> rootfs profile: $ROOTFS_PROFILE (dry-run)"
  if [ "$ROOTFS_PROFILE" = "recovery" ]; then
    echo "==> recovery package set: $RECOVERY_PKGS"
    echo "    -> install bin/shimmy + bin/shimmy-installer into rootfs (shimmy recover TUI/backend)"
    echo "    -> autologin root on tty1; launch: cage -s -- /usr/local/bin/shimmy-installer"
    echo "    -> devices: bluez/bluez-utils (CoreBluetooth HCI) + usbutils (IOKit /dev/bus/usb)"
  elif [ "$INSTALL_DESKTOP" = "1" ]; then
    echo "==> desktop profile '$ROOTFS_PROFILE' (desktop=$DESKTOP)"
    echo "==> package set: $DESKTOP_PKGS $EXTRA_PKGS"
  else
    echo "==> bare rootfs profile '$ROOTFS_PROFILE' — no packages preinstalled (boots to TTY)"
  fi
  echo "==> would run: (cd \"$HERE\" && ./build.sh gui && ./build.sh cli)"
  echo "==> would run: curl -sL \"$ARCH_URL\" -o /tmp/$ARCH_TARBALL ; tar -xzf /tmp/$ARCH_TARBALL -C \"$ROOTFS\""
  echo "==> would run: shim_cli \"$ROOTFS\" /usr/bin/pacman -Sy --noconfirm"
  if [ "$ROOTFS_PROFILE" = "recovery" ]; then
    echo "==> would run: shim_cli \"$ROOTFS\" /usr/bin/pacman -S --noconfirm $RECOVERY_PKGS"
  elif [ "$INSTALL_DESKTOP" = "1" ]; then
    echo "==> would run: shim_cli \"$ROOTFS\" /usr/bin/pacman -S --noconfirm $DESKTOP_PKGS"
    echo "==> would run: shim_cli \"$ROOTFS\" /usr/bin/pacman -S --noconfirm $EXTRA_PKGS"
  fi
  exit 0
fi

# --- 3. build the shim --------------------------------------------------------
echo "==> building the shim"
(cd "$HERE" && ./build.sh gui && ./build.sh cli)

# --- 4. bootstrap the Arch rootfs (not stored; built here) --------------------
if [ ! -x "$ROOTFS/usr/bin/pacman" ]; then
  echo "==> downloading Arch Linux ARM aarch64 base rootfs (~830 MB)"
  [ -f "/tmp/$ARCH_TARBALL" ] || curl -sL "$ARCH_URL" -o "/tmp/$ARCH_TARBALL"
  mkdir -p "$ROOTFS"
  echo "==> extracting rootfs"
  tar -xzf "/tmp/$ARCH_TARBALL" -C "$ROOTFS" 2>/dev/null || true
fi

# --- 5. make pacman work under the shim --------------------------------------
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

# --- 6. sync + install the profile's package set ------------------------------
echo "==> pacman -Sy"
run_pacman -Sy --noconfirm

if [ "$ROOTFS_PROFILE" = "recovery" ]; then
  echo "==> installing the recovery rootfs (graphical installer + tools)"
  run_pacman -S --noconfirm $RECOVERY_PKGS
elif [ "$INSTALL_DESKTOP" = "1" ]; then
  echo "==> installing desktop '$DESKTOP' (standard Arch install)"
  run_pacman -S --noconfirm $DESKTOP_PKGS
  run_pacman -S --noconfirm $EXTRA_PKGS 2>/dev/null || true
else
  echo "==> bare rootfs profile '$ROOTFS_PROFILE' — no desktop installed (boots to TTY)"
fi

# --- 7. recovery: install the shimmy CLI + installer + launch path -------------
if [ "$ROOTFS_PROFILE" = "recovery" ]; then
  echo "==> installing the recovery shell (shimmy recover + graphical installer)"
  install -d "$ROOTFS/usr/local/bin"
  install -m 0755 "$HERE/bin/shimmy" "$ROOTFS/usr/local/bin/shimmy"
  install -m 0755 "$HERE/bin/shimmy-installer" "$ROOTFS/usr/local/bin/shimmy-installer"

  # The launch path: run the installer full-screen under cage on the shim's
  # Wayland/DRM (see docs/RECOVERY-SHELL.md).
  cat > "$ROOTFS/usr/local/bin/shimmy-installer-start" <<'EOF'
#!/bin/sh
# shimmy recovery — launch the graphical installer full-screen under cage.
exec dbus-run-session -- cage -s -- /usr/local/bin/shimmy-installer
EOF
  chmod 0755 "$ROOTFS/usr/local/bin/shimmy-installer-start"

  # Boot straight into the installer: root autologin on tty1 -> the launch script.
  install -d "$ROOTFS/etc/systemd/system/getty@tty1.service.d"
  printf '[Service]\nExecStart=\nExecStart=-/sbin/agetty --autologin root --noclear %%I $TERM\n' \
    > "$ROOTFS/etc/systemd/system/getty@tty1.service.d/autologin.conf"
  touch "$ROOTFS/root/.bash_profile"
  if ! grep -q 'shimmy-installer-start' "$ROOTFS/root/.bash_profile" 2>/dev/null; then
    cat >> "$ROOTFS/root/.bash_profile" <<'EOF'

# recovery: boot straight into the graphical installer
if [ -x /usr/local/bin/shimmy-installer-start ] && [ -t 0 ]; then
  exec /usr/local/bin/shimmy-installer-start
fi
EOF
  fi

  # Alternative: a systemd service (enabled for multi-user.target) that does the
  # same cage -> installer launch without relying on a login shell.
  cat > "$ROOTFS/etc/systemd/system/shimmy-installer.service" <<'EOF'
[Unit]
Description=shimmy recovery graphical installer
After=systemd-user-sessions.service

[Service]
Type=simple
TTYPath=/dev/tty1
ExecStart=/usr/local/bin/shimmy-installer-start
StandardInput=tty
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF
fi

# --- 8. install the rdmetal driver (so the desktop renders) -------------------
# The recovery rootfs is a headless rescue shell (no desktop), so it skips the
# GPU driver; every other profile installs it as the default GL driver.
if [ "$ROOTFS_PROFILE" = "recovery" ]; then
  echo "==> skipping rdmetal driver (recovery is a headless rescue rootfs)"
elif [ -d "$HERE/rdmetal/rdmetal-libs" ]; then
  echo "==> installing the rdmetal driver"
  install -d "$ROOTFS/usr/lib/dri"
  install -m 644 "$HERE/rdmetal/rdmetal-libs/dri/rdmetal_dri.so" "$ROOTFS/usr/lib/dri/rdmetal_dri.so"
  for lib in libEGL.so.1.0.0 libGLESv2.so.2.0.0 libgbm.so.1.0.0 libglapi.so.0.0.0; do
    [ -f "$HERE/rdmetal/rdmetal-libs/$lib" ] && install -m 644 "$HERE/rdmetal/rdmetal-libs/$lib" "$ROOTFS/usr/lib/$lib"
  done
  # repoint the versioned symlinks to our libs
  ln -sf libEGL.so.1.0.0   "$ROOTFS/usr/lib/libEGL.so.1"
  ln -sf libGLESv2.so.2.0.0 "$ROOTFS/usr/lib/libGLESv2.so.2"
  ln -sf libgbm.so.1.0.0   "$ROOTFS/usr/lib/libgbm.so.1"
  ln -sf libglapi.so.0.0.0 "$ROOTFS/usr/lib/libglapi.so.0"
  echo "    -> rdmetal_dri.so + EGL/GLES/gbm libs installed; default driver (no env vars)"
else
  echo "    ! rdmetal/rdmetal-libs/ not found — run the rdmetal build first (see shimmy-rdmetal)"
fi

echo ""
echo "==> done."
echo "    rootfs profile: $ROOTFS_PROFILE"
if [ "$ROOTFS_PROFILE" = "recovery" ]; then
  echo "    run:  ./run.sh            # boots straight into the graphical installer"
  echo "    or:   ./shim_cli rootfs /usr/local/bin/shimmy-installer-start"
elif [ "$INSTALL_DESKTOP" = "1" ]; then
  echo "    run:  ./run.sh --desktop $DESKTOP"
else
  echo "    run:  ./run.sh            # bare base; install a desktop via pacman first"
fi
