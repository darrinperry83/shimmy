#!/bin/sh
# Installed as a system LaunchDaemon only in the disposable VM.
set -eu
[ "$(sysctl -n kern.hv_vmm_present)" = 1 ] || exit 1
[ "$(id -u)" = 0 ] || exit 1
base=/Library/RDXNU-P0
log=/var/tmp/rdxnu-elf.log
exec > "$log" 2>&1
export_log() {
    shared='/Volumes/My Shared Files/results'
    if [ ! -d "$shared" ]; then
        mkdir -p /var/tmp/rdxnu-results
        /sbin/mount_virtiofs com.apple.virtio-fs.automount /var/tmp/rdxnu-results || true
        shared=/var/tmp/rdxnu-results/results
    fi
    if [ -d "$shared" ]; then
        cp "$log" "$shared/elf-boot.log" || true
    fi
}
trap export_log EXIT
uname -a
sw_vers
sysctl kern.uuid kern.kernelcacheuuid kern.bootargs kern.hv_vmm_present
[ "$(sysctl -n kern.uuid)" = "$1" ]
case "$(uname -v)" in
    *DEVELOPMENT_ARM64_VMAPPLE*) ;;
    *) echo P0_WRONG_KERNEL_CONFIGURATION; exit 1 ;;
esac
launchctl print-disabled system
"$base/probe" --expect-kernel
"$base/elf-run-probe" "$base/elf-probe"
# Allow normal system services time to start before checking the headless profile.
sleep 20
ps -axo pid,comm
if pgrep -x WindowServer; then
    echo P0_WINDOWSERVER_PRESENT
    exit 1
fi
if pgrep -x loginwindow; then
    echo P0_LOGINWINDOW_PRESENT
    exit 1
fi
launchctl print system/com.apple.powerd || true
echo VM_KERNEL_PROBE_PASS
echo WINDOWSERVER_ABSENT_AT_CHECK_PASS
