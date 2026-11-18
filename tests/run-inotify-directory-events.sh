#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
root=$(mktemp -d "${TMPDIR:-/tmp}/shimmy-inotify-directory-XXXXXX")
trap 'rm -rf "$root"' EXIT HUP INT TERM
clang -Wall -Wextra -Werror -Isrc src/inotify_watch.c tests/test_inotify_directory_events.c -o "$root/host-test"
"$root/host-test"
clang -Wall -Wextra -Werror -Isrc src/inotify_watch.c tests/test_inotify_final_close.c -o "$root/close-test"
"$root/close-test"
