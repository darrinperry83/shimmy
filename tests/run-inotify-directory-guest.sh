#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
root=$(mktemp -d "${TMPDIR:-/tmp}/shimmy-inotify-directory-guest-XXXXXX")
trap 'rm -rf "$root"' EXIT HUP INT TERM
mkdir "$root/watchdir"
ZIG_GLOBAL_CACHE_DIR="$root/zig-cache" ZIG_LOCAL_CACHE_DIR="$root/zig-local" \
  zig cc -target aarch64-linux-musl -static-pie -fPIE -O2 \
  tests/guest_inotify_directory_events.c -o "$root/test"
./shim_cli "$root" /test
