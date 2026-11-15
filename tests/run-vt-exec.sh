#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
clang -Wall -Wextra -O0 -Isrc tests/vt_exec.c src/vt.c src/console.c -o build/vt_exec_test
exec ./build/vt_exec_test
