#!/bin/sh
# Stage a VM-only boot probe while retaining launchd and Apple system services.
set -eu
[ "$(sysctl -n kern.hv_vmm_present)" = 1 ] || exit 1
case "$(uname -v)" in *ARM64_VMAPPLE*) ;; *) exit 1 ;; esac
[ "$(id -u)" = 0 ] || { echo 'Run this script through sudo inside the VM'; exit 1; }
[ "$(sw_vers -buildVersion)" = 25F80 ] || exit 1
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
dest=/Library/RDXNU-P0
job=/Library/LaunchDaemons/org.rdxnu.p0.plist
# Refuse accidental replacement; remove only our test job explicitly to retry.
[ ! -e "$job" ] || { echo 'P0 already staged'; exit 1; }
mkdir -p "$dest"
launchctl print-disabled system > "$dest/disabled-before.txt"
cp "$base/p0-probe" "$dest/probe"
cp "$base/guest-p0-boot.sh" "$dest/boot.sh"
chmod 755 "$dest/probe" "$dest/boot.sh"
cp "$base/p0-job.plist" "$job"
chown root:wheel "$job" "$dest/probe" "$dest/boot.sh"
chmod 644 "$job"
plutil -lint "$job"
# Persistent overrides take effect next boot; do not kill the active UI.
launchctl disable system/com.apple.WindowServer
launchctl disable system/com.apple.loginwindow
launchctl print-disabled system
echo P0_HEADLESS_PROFILE_STAGED
