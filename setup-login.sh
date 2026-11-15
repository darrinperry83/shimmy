#!/usr/bin/env bash
# Create a normal Arch login in the selected Shimmy rootfs. Password entry stays
# on the local terminal; Shimmy never chooses, stores, or prints a password.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="${ROOTFS:-$HERE/rootfs}"
SHIM="$HERE/shim_cli"

[ -x "$SHIM" ] && [ -x "$ROOT/usr/bin/useradd" ] && [ -x "$ROOT/usr/bin/chpasswd" ] || {
  echo "Build Shimmy and install the Arch rootfs first." >&2; exit 1;
}
[ -t 0 ] && [ -t 1 ] || { echo "Run this from an interactive terminal." >&2; exit 1; }

default_user="$(id -un)"
read -r -p "Linux username [$default_user]: " username
username="${username:-$default_user}"
[[ "$username" =~ ^[a-z_][a-z0-9_-]{0,31}$ ]] && [ "$username" != root ] || {
  echo "Choose a lowercase Linux username (not root)." >&2; exit 1;
}

if ! awk -F: -v user="$username" '$1 == user {found=1} END {exit !found}' "$ROOT/etc/passwd"; then
  RDUID0=1 "$SHIM" "$ROOT" /usr/bin/useradd -m -s /bin/bash "$username"
fi

read -r -s -p "Linux password: " password </dev/tty
printf '\n'
read -r -s -p "Repeat Linux password: " confirm </dev/tty
printf '\n'
[ -n "$password" ] && [ "$password" = "$confirm" ] || {
  echo "Passwords did not match or were empty." >&2; exit 1;
}
printf '%s:%s\n' "$username" "$password" | RDUID0=1 "$SHIM" "$ROOT" /usr/bin/chpasswd
unset password confirm
echo "Linux account '$username' is ready. Sign in at a Shimmy TTY with that password."
