#!/bin/sh
# Run both the host state test and the real ARM64 guest syscall dispatch test.
set -eu
cd "$(dirname "$0")/.."
root=$(mktemp -d "${TMPDIR:-/tmp}/shimmy-inotify-28-XXXXXX")
trap 'rm -rf "$root"' EXIT HUP INT TERM
clang -Wall -Wextra -Werror -Isrc src/inotify_watch.c tests/test_inotify_watch.c -o "$root/host-test"
"$root/host-test"
touch "$root/watch"
ZIG_GLOBAL_CACHE_DIR="$root/zig-cache" ZIG_LOCAL_CACHE_DIR="$root/zig-local" \
  zig cc -target aarch64-linux-musl -static-pie -fPIE -O2 \
  tests/guest_inotify_rm_watch.c -o "$root/test"
./shim_cli "$root" /test
