#!/bin/sh
# Disposable VM only. Capture crash evidence without altering boot policy.
set -eu
[ "$(sysctl -n kern.hv_vmm_present)" = 1 ] || exit 1
case "$(uname -v)" in *ARM64_VMAPPLE*) ;; *) exit 1 ;; esac
share='/Volumes/My Shared Files/results'
nvram -x -p > "$share/core-nvram.plist"
find /Volumes/Data/Library/Logs/DiagnosticReports -type f -name '*panic*' -print > "$share/core-panic-paths.txt" 2>/dev/null || true
find /Volumes/Data/Library/Logs/DiagnosticReports -type f -name '*.ips' -mtime -1 -print >> "$share/core-panic-paths.txt" 2>/dev/null || true
mkdir -p "$share/core-crashes"
while IFS= read -r path; do cp "$path" "$share/core-crashes/"; done < "$share/core-panic-paths.txt"
echo CORE_CRASH_EVIDENCE_EXPORTED
