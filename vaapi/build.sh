#!/bin/bash
# Cross-compile the guest VAAPI driver + decode probe for aarch64 against the Arch rootfs.
#   ./build.sh            # build both, install into the rootfs (idempotent)
#   ./build.sh --noinstall # just build (artifacts land in ./out)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="${ROOTFS:-$HERE/../../rootfs-arch}"
CC=/opt/homebrew/opt/llvm/bin/clang
GCCDIR="$ROOT/usr/lib/gcc/aarch64-unknown-linux-gnu/16.1.1"
OUT="$HERE/out"

mkdir -p "$OUT"
CFLAGS=(
  --target=aarch64-unknown-linux-gnu
  --sysroot="$ROOT"
  -I"$ROOT/usr/include"
  -fuse-ld=lld
  -B"$GCCDIR"
  -L"$ROOT/usr/lib" -L"$ROOT/lib"
  -O2
)

echo "== rdva_drv_video.so (guest libva backend) =="
"$CC" "${CFLAGS[@]}" -shared -fPIC -Wl,-soname,rdva_drv_video.so \
  -o "$OUT/rdva_drv_video.so" "$HERE/rdva_drv_video.c"

echo "== rdva_probe (guest decode probe) =="
"$CC" "${CFLAGS[@]}" -o "$OUT/rdva_probe" "$HERE/rdva_probe.c" -lva -lva-drm

echo "== artifacts =="
file "$OUT/rdva_drv_video.so" "$OUT/rdva_probe"
nm -D "$OUT/rdva_drv_video.so" | grep -E '__vaDriverInit| T ' | head

if [ "${1:-}" != "--noinstall" ]; then
  echo "== install into rootfs =="
  install -m 0755 "$OUT/rdva_drv_video.so" "$ROOT/usr/lib/dri/rdva_drv_video.so"
  install -m 0755 "$OUT/rdva_probe"      "$ROOT/usr/local/bin/rdva_probe"
  install -m 0644 "$HERE/test.h264"      "$ROOT/root/test.h264"
  echo "installed: $ROOT/usr/lib/dri/rdva_drv_video.so, $ROOT/usr/local/bin/rdva_probe, $ROOT/root/test.h264"
fi
echo "done."
