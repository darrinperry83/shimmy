#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
probe="$(mktemp /tmp/shimmy-mqueue-fd.XXXXXX)"
trap 'rm -f "$probe"' EXIT HUP INT TERM
clang -arch arm64 -Wall -Wextra -Werror -pthread -Isrc \
  tests/test-mqueue-fd-registry.c src/mqueue_core.c src/mqueue_fd_registry.c -o "$probe"
"$probe"
