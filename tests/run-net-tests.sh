#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
binary=$(mktemp /tmp/shimmy-net-tests.XXXXXX)
trap 'rm -f "$binary"' EXIT HUP INT TERM
clang -arch arm64 -O2 -Wall -o "$binary" \
  "$repo/src/test_net.c" "$repo/src/net.c" \
  "$repo/src/netlink_route_transport.c" "$repo/src/netlink_route_snapshot.c" \
  "$repo/src/guest_cred_registry.c" "$repo/src/pidfd_shim.c" \
  "$repo/src/snd_alsa.c" "$repo/src/coreaudio_host.c" \
  "$repo/tests/test_net_stubs.c" \
  -framework AudioUnit -framework CoreAudio -framework AudioToolbox
"$binary"
