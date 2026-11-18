#!/usr/bin/env bash
# Install a built rdmetal Mesa driver into an existing Arch root without
# rebuilding or changing its other packages.  A fresh install uses this too.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${ROOTFS:-$HERE/rootfs}"
DRIVER="${RDMETAL_DRIVER:-$HERE/rdmetal/rdmetal-libs/dri/rdmetal_dri.so}"
CHECK=0
while [ "$#" -gt 0 ]; do
  case "$1" in
    --root) [ "$#" -ge 2 ] || { echo "--root needs a directory" >&2; exit 2; }; ROOT="$2"; shift 2 ;;
    --driver) [ "$#" -ge 2 ] || { echo "--driver needs a file" >&2; exit 2; }; DRIVER="$2"; shift 2 ;;
    --check) CHECK=1; shift ;;
    *) echo "usage: $0 [--root ARCH_ROOT] [--driver RDMETAL_DRI_SO] [--check]" >&2; exit 2 ;;
  esac
done

[ -f "$DRIVER" ] && [ -s "$DRIVER" ] || { echo "missing rdmetal driver: $DRIVER" >&2; exit 1; }
[ -d "$ROOT/usr/lib" ] || { echo "not an installed Arch root: $ROOT" >&2; exit 1; }
case "$(file -b "$DRIVER")" in
  *ELF*shared\ object*ARM\ aarch64*|*ELF*shared\ object*ARM64*) ;;
  *) echo "driver is not an aarch64 ELF shared object: $DRIVER" >&2; exit 1 ;;
esac

TARGET="$ROOT/usr/lib/dri/rdmetal_dri.so"
[ ! -L "$TARGET" ] || { echo "refusing symlink target: $TARGET" >&2; exit 1; }
source_hash="$(shasum -a 256 "$DRIVER" | awk '{print $1}')"
target_hash=""
if [ -f "$TARGET" ]; then
  target_hash="$(shasum -a 256 "$TARGET" | awk '{print $1}')"
fi
if [ "$CHECK" = 1 ]; then
  echo "source $source_hash  $DRIVER"
  echo "root   ${target_hash:-missing}  $TARGET"
  [ "$target_hash" = "$source_hash" ]
  exit $?
fi
if [ "$target_hash" = "$source_hash" ]; then
  echo "rdmetal already current: $source_hash"
  exit 0
fi

install -d "$ROOT/usr/lib/dri"
if [ -n "$target_hash" ]; then
  backup="$TARGET.previous.${target_hash:0:12}"
  [ -e "$backup" ] || cp -p "$TARGET" "$backup"
  echo "previous driver: $backup"
fi
temporary="$(mktemp "$ROOT/usr/lib/dri/.rdmetal_dri.so.XXXXXX")"
trap 'rm -f "$temporary"' EXIT
install -m 755 "$DRIVER" "$temporary"
[ "$(shasum -a 256 "$temporary" | awk '{print $1}')" = "$source_hash" ] || {
  echo "staged driver hash mismatch" >&2; exit 1;
}
mv -f "$temporary" "$TARGET"
trap - EXIT
[ "$(shasum -a 256 "$TARGET" | awk '{print $1}')" = "$source_hash" ] || {
  echo "installed driver hash mismatch" >&2; exit 1;
}
echo "installed rdmetal: $source_hash  $TARGET"
