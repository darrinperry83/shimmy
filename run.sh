#!/usr/bin/env bash
# shimmy — run the desktop.
#   ./run.sh                 # Hyprland (default)
#   ./run.sh --desktop kde   # KDE Plasma
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DESKTOP="hyprland"
for a in "$@"; do case "$a" in --desktop) DESKTOP="${2:-hyprland}";; esac; done

export RDSOCKETGROUP=/tmp/rdx-shared RDUNIXPREFIX=/tmp/rdx-shared RDDRM=1

case "$DESKTOP" in
  kde|plasma)
    # KDE: dbus session -> kwin_wayland -> plasmashell (see ARCH_MIGRATION.md step 17)
    exec RDUID0=1 "$HERE/shim_gui" rootfs /bin/bash -c '
      dbus-daemon --session --nofork --print-address > /tmp/dbus-addr &
      sleep 0.5
      export DBUS_SESSION_BUS_ADDRESS=$(cat /tmp/dbus-addr)
      WAYLAND_DISPLAY=wayland-0 kwin_wayland --socket wayland-1 &
      sleep 2
      QT_QUICK_BACKEND=software QMLSCENE_DEVICE=softwarecontext WAYLAND_DISPLAY=wayland-1 plasmashell'
    ;;
  *)
    exec "$HERE/shim_gui" rootfs /usr/bin/Hyprland \
      --config "$HERE/hypr.lua" --i-am-really-stupid
    ;;
esac
