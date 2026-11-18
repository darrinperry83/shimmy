#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
probe="$(mktemp /tmp/shimmy-mqueue-core.XXXXXX)"
trap 'rm -f "$probe"' EXIT HUP INT TERM
clang -arch arm64 -Wall -Wextra -Werror -pthread -Isrc \
  tests/test-mqueue-core.c src/mqueue_core.c -o "$probe"
"$probe"
