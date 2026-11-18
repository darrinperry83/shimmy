#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
probe="$(mktemp /tmp/shimmy-mqueue-fork-lock-test.XXXXXX)"
trap 'rm -f "$probe"' EXIT HUP INT TERM
clang -arch arm64 -Wall -Wextra -Werror -pthread -Isrc \
  tests/test-mqueue-fork-lock.c src/mqueue_shared.c -o "$probe"
"$probe"
