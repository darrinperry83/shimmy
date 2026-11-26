#!/bin/sh
# Run inside the disposable macOS guest after configuring its custom kernel.
# Read-only apart from the isolated child created by the probe.
set -eu
if [ "$#" -ne 2 ]; then
    echo "usage: vm_probe.sh EXPECTED_KERNEL_UUID PROBE_BINARY" >&2
    exit 2
fi
expected_uuid=$1
probe=$2
if [ "$(sysctl -n kern.hv_vmm_present)" != 1 ]; then
    echo "VM_PROBE_REFUSED: this is not a virtual machine" >&2
    exit 1
fi
case "$(uname -v)" in
    *DEVELOPMENT_ARM64_VMAPPLE*) ;;
    *) echo "VM_PROBE_REFUSED: expected DEVELOPMENT_ARM64_VMAPPLE" >&2; exit 1 ;;
esac
if [ "$(sw_vers -buildVersion)" != 25F80 ]; then
    echo "VM_PROBE_REFUSED: guest must match build 25F80" >&2
    exit 1
fi
if [ "$(sysctl -n kern.uuid)" != "$expected_uuid" ]; then
    echo "VM_PROBE_REFUSED: running kernel UUID differs from the audited build" >&2
    exit 1
fi
uname -v
sysctl kern.uuid kern.bootargs kern.hv_vmm_present
"$probe" --expect-kernel
echo "VM_KERNEL_PROBE_PASS"
