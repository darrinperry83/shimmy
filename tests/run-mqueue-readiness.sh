#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
probe="$(mktemp /tmp/shimmy-mqueue-readiness.XXXXXX)"
trap 'rm -f "$probe"' EXIT HUP INT TERM
clang -arch arm64 -Wall -Wextra -Werror -Isrc \
  tests/test-mqueue-readiness.c src/mqueue_shared.c -o "$probe"
"$probe"
