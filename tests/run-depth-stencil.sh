#!/usr/bin/env bash
# Run after ./build.sh cli (or gui) to use the current vendored host objects.
# Fixture SPIR-V: glslangValidator -V tests/fixtures/depth-stencil/{triangle.vert,red.frag,green.frag}
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d /tmp/shimmy-depth-stencil-XXXXXX)
trap 'rm -rf "$work"' EXIT
clang++ -arch arm64 -std=c++17 -fobjc-arc -Ithird_party -Ithird_party/spirv-cross \
  tests/test_rdmetal_depth_draw.mm build/third_party/rdmetal_resource.o \
  build/third_party/rdgpu_draw_host.o third_party/lib/libspirv-cross-msl.a \
  third_party/lib/libspirv-cross-glsl.a third_party/lib/libspirv-cross-core.a \
  -framework Metal -framework IOSurface -framework Foundation -o "$work/draw"
"$work/draw"
clang -arch arm64 -O2 -Wall -Wextra -Isrc tests/test_rdgpu_guest_depth.c \
  src/rdgpu_guest_syscall.c -framework Foundation -o "$work/guest"
"$work/guest"
clang -arch arm64 -O2 -Wall -Wextra -Isrc tests/test_rdmetal_depth.m \
  build/third_party/rdmetal_resource.o build/third_party/rdgpu_broker.o \
  -framework Foundation -framework Metal -framework IOSurface -framework Security \
  -o "$work/resource"
"$work/resource"
