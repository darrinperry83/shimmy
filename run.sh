#!/bin/bash
# run.sh — boot the rdmetal guest and run a program, or launch the Omarchy desktop.
#
#   ./run.sh                              # --rootfs arch: audio up, headless (shim_cli)
#   ./run.sh -- <guest-cmd> ...           # audio up, then run <guest-cmd>
#   ./run.sh --gui [-- <guest-cmd>]       # use shim_gui (e.g. a Hyprland/KDE desktop + audio)
#   ./run.sh --rootfs omarchy             # launch the Omarchy desktop (shim_gui)
#                                          #   Hyprland + /usr/share/omarchy/omarchy.lua,
#                                          #   quickshell bar + foot autostarted by the config
#   ./run.sh --rootfs arch --gui -- <cmd> # bare base + a user-installed desktop via <cmd>
#   ./run.sh --init [--gui|--cli]         # REAL Linux boot: exec /sbin/init (systemd); getty ->
#                                          #   login -> whatever desktop the user installed.
#                                          #   No hardcoded command and no hardcoded DE.
#
# The desktop itself is installed by the user via pacman; --rootfs only selects which
# prepared rootfs experience to boot (arch = bare base, omarchy = Omarchy experience).
# --init is the boot-to-init path: it boots systemd (the standard getty/dbus/network stack)
# instead of a hardcoded guest command, so the login -> desktop choice belongs to the guest.
#
# Env for the guest:
#   RDSND=1          enable the synthetic ALSA card (snd_alsa.c)
#   RDUID0=1         run as uid/gid 0 so dbus-daemon authenticates
#   RDSOCKETGROUP=…  a per-run shared AF_UNIX namespace: pipewire/wireplumber/pipewire-pulse
#                    (and, for the desktop, Hyprland's inner wayland-1) are each re-exec'd shim
#                    processes, so without a shared group they bind their sockets in separate
#                    per-pid /tmp/rdx-<pid> prefixes and never see each other.
#
# Requires ./install.sh to prepare rootfs; ./install-guest-extras.sh adds the optional audio/Omarchy experience.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="${ROOTFS:-$HERE/rootfs}"
SHIM="$HERE/shim_cli"
ROOTFS_TYPE="arch"
BOOT_INIT=0
SHIM_CHOSEN=0

while [ $# -gt 0 ]; do
  case "$1" in
    --init) BOOT_INIT=1; shift ;;
    --gui) SHIM="$HERE/shim_gui"; SHIM_CHOSEN=1; shift ;;
    --cli) SHIM="$HERE/shim_cli"; SHIM_CHOSEN=1; shift ;;
    --rootfs) ROOTFS_TYPE="${2:-arch}"; shift 2 ;;
    --) shift; break ;;
    *) break ;;
  esac
done

case "$ROOTFS_TYPE" in arch|omarchy) ;; *) echo "unknown --rootfs: $ROOTFS_TYPE (arch|omarchy)" >&2; exit 2 ;; esac

[ -d "$ROOT" ] || { echo "error: rootfs not found at $ROOT (set ROOTFS=...)" >&2; exit 1; }
[ -x "$SHIM" ] || { echo "error: $SHIM missing — run ./build.sh first" >&2; exit 1; }

# ---- boot to init (systemd): a real Linux boot ----------------------------------------
if [ "$BOOT_INIT" = 1 ]; then
  # No hardcoded guest command and no hardcoded desktop: the shim execs the rootfs's
  # /sbin/init (systemd), which brings up getty on the VT console (tty1..6), dbus, network,
  # etc. The user logs in and starts whatever desktop they installed (Hyprland/KDE/Sway).
  #
  # The scanout takeover (RDGPU_SCANOUT=1) is a HOST-side flag read at drm_present_fb by
  # the shim (drmasahi.c -> window.m -> rdgpu_scanout_client), so it fires on whichever
  # DRM-master compositor the chosen desktop runs. Nothing here pins a DE: RDGPU_SCANOUT
  # is inherited through `exec` unchanged.
  [ "$SHIM_CHOSEN" = 0 ] && SHIM="${SHIM_GUI_OVERRIDE:-$HERE/shim_gui}"   # app bundle may supply its signed GUI binary
  [ -x "$SHIM" ] || { echo "error: $SHIM missing — run ./build.sh gui first" >&2; exit 1; }

  # The rootfs's /tmp is already a host-backed sticky directory. Shimmy does
  # not implement a separate tmpfs mount namespace yet; accepting mount(2)
  # without making a mount makes systemd retry tmp.mount indefinitely. Mask
  # only the vendor unit, leaving any existing administrator override alone.
  [ "$(stat -f %Sp "$ROOT/tmp")" = drwxrwxrwt ] || {
    echo "error: $ROOT/tmp must be a mode-1777 directory" >&2; exit 1;
  }
  TMP_UNIT="$ROOT/etc/systemd/system/tmp.mount"
  if [ ! -e "$TMP_UNIT" ] && [ ! -L "$TMP_UNIT" ]; then
    mkdir -p "$(dirname "$TMP_UNIT")"
    ln -s /dev/null "$TMP_UNIT"
    echo "[run] masked tmp.mount; guest /tmp uses the rootfs's host-backed sticky directory" >&2
  fi

  # Older installed roots have only tty1 enabled. Install the remaining gettys
  # idempotently before PID 1 starts, preserving any administrator overrides.
  mkdir -p "$ROOT/etc/systemd/system/getty.target.wants"
  for tty in 2 3 4 5 6; do
    unit="$ROOT/etc/systemd/system/getty.target.wants/getty@tty${tty}.service"
    if [ ! -e "$unit" ] && [ ! -L "$unit" ]; then
      ln -s /usr/lib/systemd/system/getty@.service "$unit"
    fi
  done

  INIT="$ROOT/sbin/init"
  INIT_RESOLVED="$(readlink -f "$INIT" 2>/dev/null || true)"
  if [ ! -x "$INIT" ] && [ ! -x "$INIT_RESOLVED" ]; then
    echo "error: no bootable init at $ROOT/sbin/init" >&2
    echo "       install systemd + the /sbin/init symlink into the rootfs:" >&2
    echo "       RDUID0=1 $HERE/shim_cli $ROOT /usr/bin/pacman -Sy --noconfirm --disable-sandbox systemd systemd-sysvcompat" >&2
    exit 1
  fi
  echo "[run] boot-to-init: $INIT -> ${INIT_RESOLVED:-$INIT} (shim=$(basename "$SHIM"))" >&2

  # systemd must believe it is uid 0. (A full systemd-as-PID-1 boot additionally needs the
  # shim to report getpid()==1 to the init process — see the live-boot notes. That mapping
  # lives in the shim's syscall layer, not here; this script only wires the exec path.)
  # A booted display manager must establish its own seat, runtime directory and display
  # socket. build_guest_env() otherwise injects the nested shim defaults (wayland-0,
  # DISPLAY=:0, XDG_RUNTIME_DIR=/tmp and indirect GL) into PID 1 and every service it
  # starts. Empty RDENV_* entries shadow those defaults and are then removed from the
  # guest environment; the display manager can set the real values for its user session.
  RDENV_WAYLAND_DISPLAY= RDENV_DISPLAY= RDENV_XDG_RUNTIME_DIR= \
  RDENV_LIBGL_ALWAYS_INDIRECT= RDNATIVE_SESSION=1 RDUID0=1 \
    exec "$SHIM" "$ROOT" /sbin/init "$@"
fi

# ---- Omarchy desktop ------------------------------------------------------------------
if [ "$ROOTFS_TYPE" = omarchy ]; then
  SHIM="$HERE/shim_gui"   # the desktop needs the Metal window / in-process compositor
  [ -x "$SHIM" ] || { echo "error: $SHIM missing — run ./build.sh gui first" >&2; exit 1; }
  [ -x "$ROOT/usr/bin/Hyprland" ] || { echo "error: Hyprland missing — run ./install.sh --rootfs omarchy first" >&2; exit 1; }
  [ -f "$ROOT/usr/share/omarchy/omarchy.lua" ] || { echo "error: omarchy.lua missing — run ./install.sh --rootfs omarchy first" >&2; exit 1; }

  GRP="$(mktemp -d /tmp/rdmeta-omarchy-XXXXXX)" || exit 1
  chmod 700 "$GRP"
  echo "[run] Omarchy desktop: RDSOCKETGROUP=$GRP" >&2

  # Native DRM-master desktop: Hyprland's aquamarine DRM backend runs as the sole
  # compositor (libseat 'noop' grants the seat with no seatd/logind). RDENV_WAYLAND_DISPLAY
  # is deliberately UNSET so aquamarine takes the DRM backend against the rdmetal node
  # instead of connecting as a nested Wayland client of the shim's wayland-0; the shim's
  # wlsrv then refuses the nested-Wayland backend connection, so there is no
  # "wayland-1 on the other screen". With RDGPU_SCANOUT=1 (takeover) the SAME DRM-master
  # flip is what drm_present_fb stages and window.m hands to the Dock's direct-scanout
  # service — no nested wayland-0 involved. Its inner wayland-1 socket (where
  # quickshell/foot connect) is shared via RDSOCKETGROUP/RDUNIXPREFIX.
  # Route the compositor's clear/draw through the in-process host Metal broker
  # (rdmetal direct mode) instead of the softpipe fallback.  Host-side vars turn
  # on the shim syscall-0x2002 broker (rdgpu_mock_gate.c) + the same-image Metal
  # fixture; RDENV_* vars are forwarded to the guest driver (rdmetal_screen.c).
  RDMETAL_LEDGER_MOCK=1 RDMETAL_SAME_IMAGE_METAL_FIXTURE=1 \
  RDMETAL_VIRTUAL_DMABUF_TEST=1 \
  RDSOCKETGROUP="$GRP" RDUNIXPREFIX="$GRP" \
  RDENV_XDG_RUNTIME_DIR=/tmp \
  RDENV_LIBSEAT_BACKEND=noop \
  RDENV_RDMETAL_DIRECT_RESOURCE=1 RDENV_RDMETAL_SHIM_OP5_MOCK=1 \
  RDENV_RDMETAL_USE_DMABUF=1 RDENV_RDMETAL_VIRTUAL_DMABUF_TEST=1 \
  WLSRV_DEBUG=1 RDDRM_TRACE=1 \
  exec "$SHIM" "$ROOT" /usr/bin/Hyprland --config /usr/share/omarchy/omarchy.lua --i-am-really-stupid
fi

# ---- bare base: audio stack + optional guest command -----------------------------------
BOOT="$ROOT/usr/local/bin/rdmetal-audio-boot.sh"
[ -x "$BOOT" ] || { echo "error: $BOOT missing — run ./install.sh first" >&2; exit 1; }

GRP="$(mktemp -d /tmp/rdmeta-audio-XXXXXX)" || exit 1
echo "[run] RDSOCKETGROUP=$GRP" >&2

RDSND=1 RDUID0=1 RDSOCKETGROUP="$GRP" exec "$SHIM" "$ROOT" /usr/local/bin/rdmetal-audio-boot.sh "$@"
