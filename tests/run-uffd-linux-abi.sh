#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
root=${RD_UFFD_SYSROOT:-/Users/dperry/Documents/Codex/2026-11-15/task-3/shimmy-boot-fixture}
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT
clang -std=c11 -Wall -Wextra -Werror -pthread -Isrc -idirafter "$root/usr/include" \
  src/uffd_backend.c src/uffd_linux_abi.c tests/test_uffd_linux_abi.c -o "$tmpdir/test-uffd-linux-abi"
"$tmpdir/test-uffd-linux-abi"
