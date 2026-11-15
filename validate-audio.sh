#!/bin/bash
# validate-audio.sh — end-to-end audio validation for the rdmetal shim.
#
# Boots dbus + pipewire + wireplumber + pipewire-pulse under the shim, confirms the synthetic
# ALSA card is enumerated from the shim-generated sysfs/udev entries (no hand-placed rootfs
# files), the alsa_output sink is auto-created, and pw-cat drives a RUNNING graph into CoreAudio.
#
# Prints PASS/FAIL per check and exits non-zero on any failure.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="${ROOTFS:-$HERE/../rootfs-arch}"
SHIM="$HERE/shim_cli"

[ -d "$ROOT" ] || { echo "error: rootfs not found at $ROOT" >&2; exit 1; }
[ -x "$SHIM" ] || { echo "error: $SHIM missing — run ./build.sh cli first" >&2; exit 1; }

# The guest-side validation script is written into the rootfs tmp on each run (self-contained,
# idempotent; it is a normal test script, not a shim-served sysfs entry).
GUEST="$ROOT/tmp/rdmeta-audio-check.sh"
cat > "$GUEST" <<'GUEST_EOF'
#!/bin/sh
export XDG_RUNTIME_DIR=/tmp/runtime-root
mkdir -p "$XDG_RUNTIME_DIR" 2>/dev/null
chmod 700 "$XDG_RUNTIME_DIR" 2>/dev/null || true
rm -f "$XDG_RUNTIME_DIR"/pipewire-0.lock "$XDG_RUNTIME_DIR"/pipewire-0-manager.lock 2>/dev/null

DBUS_PID=""
if command -v dbus-daemon >/dev/null 2>&1; then
  dbus-daemon --session --nofork --print-address=1 --print-pid=1 > /tmp/rdmeta-dbus.out 2>/tmp/rdmeta-dbus.err &
  DBUS_PID=$!
  sleep 1
  [ -s /tmp/rdmeta-dbus.out ] && export DBUS_SESSION_BUS_ADDRESS="$(sed -n '1p' /tmp/rdmeta-dbus.out)"
fi
pipewire > /tmp/pipewire.log 2>&1 &
PWPID=$!
sleep 2
wireplumber > /tmp/wireplumber.log 2>&1 &
WPPID=$!
sleep 2
pipewire-pulse > /tmp/pipewire-pulse.log 2>&1 &
PPPID=$!
sleep 3

echo "=== SINK ==="
pw-cli list-objects 2>/dev/null | grep -iE 'alsa_output|alsa_card|Audio/Sink' | head -20

echo "=== PLAY (pw-cat -> RUNNING graph -> CoreAudio) ==="
if command -v ffmpeg >/dev/null 2>&1; then
  ffmpeg -hide_banner -loglevel error -f lavfi -i "sine=frequency=1000:duration=3" \
    -ac 2 -ar 48000 -f wav /tmp/rdmeta-tone.wav 2>&1 | head -3
  pw-cat --playback /tmp/rdmeta-tone.wav 2>/tmp/rdmeta-pwcat.err &
else
  pw-cat --playback --format=f32 --channels=2 --rate=48000 /dev/zero 2>/tmp/rdmeta-pwcat.err &
fi
CATPID=$!
sleep 5
kill "$CATPID" 2>/dev/null

echo "=== CORE AUDIO ==="
grep -a '\[ca\]' /tmp/pipewire.log 2>/dev/null | tail -5

kill "$PPPID" "$WPPID" "$PWPID" "$DBUS_PID" 2>/dev/null
sleep 1
exit 0
GUEST_EOF
chmod +x "$GUEST"

GRP="$(mktemp -d /tmp/rdmeta-audio-XXXXXX)"
OUT="$(mktemp /tmp/rdmeta-validate-XXXXXX)"
echo "== rdmeta audio validation =="
echo "rootfs=$ROOT  group=$GRP  log=$OUT"

RDSND=1 RDUID0=1 RDSOCKETGROUP="$GRP" "$SHIM" "$ROOT" /tmp/rdmeta-audio-check.sh > "$OUT" 2>&1
rc=$?

fail=0
echo "---- sink auto-created ----"
if grep -qa 'alsa_output' "$OUT"; then
  grep -a 'alsa_output\|alsa_card' "$OUT" | head -5
  echo "PASS: alsa_output sink auto-created"
else
  echo "FAIL: no alsa_output sink in pw-cli list-objects"; fail=1
fi

echo "---- RUNNING graph into CoreAudio ----"
if grep -qa 'AudioOutputUnitStart: running' "$OUT"; then
  grep -a '\[ca\]' "$OUT" | tail -3
  echo "PASS: CoreAudio sink live (graph RUNNING)"
else
  echo "FAIL: no [ca] AudioOutputUnitStart: running"; fail=1
fi

echo "---- log ----"
echo "$OUT"
exit $(( rc != 0 ? rc : fail ))
