#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
root=$(mktemp -d "${TMPDIR:-/tmp}/shimmy-inotify-file-XXXXXX")
trap 'rm -rf "$root"' EXIT HUP INT TERM
clang -Wall -Wextra -Werror -Isrc src/inotify_watch.c tests/test_inotify_file_events.c -o "$root/host-test"
"$root/host-test"
clang -Wall -Wextra -Werror -Isrc src/inotify_watch.c tests/test_inotify_worker_failure.c -o "$root/failure-test"
"$root/failure-test"
