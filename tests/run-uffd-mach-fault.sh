#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT
clang -std=c11 -Wall -Wextra -Werror -pthread -Isrc \
  src/uffd_backend.c src/uffd_linux_abi.c tests/test_uffd_mach_fault.c build/mach_excServer.c \
  -o "$tmpdir/test-uffd-mach-fault"
"$tmpdir/test-uffd-mach-fault"
