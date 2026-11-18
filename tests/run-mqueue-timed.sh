#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
probe="$(mktemp /tmp/shimmy-mqueue-timed.XXXXXX)"
trap 'rm -f "$probe"' EXIT HUP INT TERM
clang -arch arm64 -Wall -Wextra -Werror -pthread -Isrc \
  tests/test-mqueue-timed.c src/mqueue_core.c -o "$probe"
"$probe"
