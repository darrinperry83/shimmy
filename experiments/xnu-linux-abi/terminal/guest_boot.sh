#!/bin/sh
# Root LaunchDaemon in the disposable VM only; no host or physical boot changes.
set -eu
[ "$(sysctl -n kern.hv_vmm_present)" = 1 ]
[ "$(id -u)" = 0 ]
base=/Library/RDXNU-P0
log=/var/tmp/rdxnu-terminal.log
exec > "$log" 2>&1
export_logs() {
    shared='/Volumes/My Shared Files/results'
    if [ ! -d "$shared" ]; then
        mkdir -p /var/tmp/rdxnu-results
        /sbin/mount_virtiofs com.apple.virtio-fs.automount /var/tmp/rdxnu-results || true
        shared=/var/tmp/rdxnu-results/results
    fi
    if [ -d "$shared" ]; then
        cp "$log" "$shared/terminal-boot.log" || true
        if [ -f /var/tmp/rdxnu-terminal-display.log ]; then
            cp /var/tmp/rdxnu-terminal-display.log "$shared/terminal-display.log" || true
        fi
    fi
}
trap export_logs EXIT
uname -a
sysctl kern.uuid kern.kernelcacheuuid kern.bootargs
[ "$(sysctl -n kern.uuid)" = "$1" ]
case "$(uname -v)" in *DEVELOPMENT_ARM64_VMAPPLE*) ;; *) exit 1 ;; esac
"$base/probe" --expect-kernel
"$base/elf-run-probe" "$base/elf-probe"
"$base/run-arch" "$base/arch-ld.so" --help
shasum -a 256 "$base/read-probe"
"$base/run-pty" "$base/read-probe"
sleep 10
if pgrep -x WindowServer; then echo WINDOWSERVER_PRESENT; exit 1; fi
if pgrep -x loginwindow; then echo LOGINWINDOW_PRESENT; exit 1; fi
echo TERMINAL_REGRESSIONS_PASS
echo WINDOWSERVER_ABSENT_AT_CHECK_PASS
"$base/display-output" "$base/run-pty" "$base/read-probe" > /var/tmp/rdxnu-terminal-display.log 2>&1 &
display=$!
sleep 3
export_logs
wait "$display"
echo TERMINAL_DISPLAY_PROCESS_PASS
