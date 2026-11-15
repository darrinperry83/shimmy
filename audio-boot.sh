#!/bin/sh
# audio-boot.sh — guest-side boot of the rdmetal audio stack.
#
# Started by run.sh with RDSND=1 (enable the synthetic ALSA card) and RDUID0=1 (so dbus-daemon
# can authenticate). Boots the session D-Bus bus, pipewire, wireplumber and pipewire-pulse,
# waits for WirePlumber to auto-create the alsa_output sink from the shim's synthetic ALSA card,
# then runs the given command (or keeps the audio stack alive).
#
# This script is the source of truth in the shim tree; install.sh copies it into the rootfs as
# /usr/local/bin/rdmetal-audio-boot.sh so run.sh can exec it as a guest program.
set -u

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/runtime-root}"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR" 2>/dev/null || true
rm -f "$XDG_RUNTIME_DIR/pipewire-0.lock" "$XDG_RUNTIME_DIR/pipewire-0-manager.lock" 2>/dev/null

DBUS_PID=""
PW_PID=""
WP_PID=""
PP_PID=""

# D-Bus session bus (best-effort: the ALSA sink path does not require it, but pipewire-pulse and
# many audio clients do). Run with --nofork + captured address so the bus lives with this shell.
if command -v dbus-daemon >/dev/null 2>&1; then
  dbus-daemon --session --nofork --print-address=1 --print-pid=1 \
    > /tmp/rdmeta-dbus.out 2>/tmp/rdmeta-dbus.err &
  DBUS_PID=$!
  sleep 1
  if [ -s /tmp/rdmeta-dbus.out ]; then
    export DBUS_SESSION_BUS_ADDRESS="$(sed -n '1p' /tmp/rdmeta-dbus.out)"
    echo "[audio] DBUS_SESSION_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS" >&2
  fi
fi

pipewire > /tmp/pipewire.log 2>&1 &
PW_PID=$!
sleep 2
wireplumber > /tmp/wireplumber.log 2>&1 &
WP_PID=$!
sleep 2
pipewire-pulse > /tmp/pipewire-pulse.log 2>&1 &
PP_PID=$!

# Wait up to ~10s for WirePlumber to auto-create the alsa_output sink from the ALSA card.
i=0
while [ "$i" -lt 20 ]; do
  if pw-cli list-objects 2>/dev/null | grep -q 'alsa_output'; then
    echo "[audio] ALSA sink auto-created" >&2
    break
  fi
  sleep 0.5
  i=$((i + 1))
done

if [ "$#" -gt 0 ]; then
  "$@"
  rc=$?
  kill "$PP_PID" "$WP_PID" "$PW_PID" "$DBUS_PID" 2>/dev/null
  exit $rc
fi

echo "[audio] stack up; keeping alive" >&2
while :; do sleep 60; done
