#!/bin/sh
# Run only inside the disposable VM's recoveryOS after guest-policy approval.
set -eu
if [ "$(sysctl -n kern.hv_vmm_present)" != 1 ]; then
    echo 'REFUSED: custom boot configuration is restricted to the VM' >&2
    exit 1
fi
case "$(uname -v)" in
    *ARM64_VMAPPLE*) ;;
    *) echo 'REFUSED: expected the VMAPPLE platform' >&2; exit 1 ;;
esac
if [ "${1:-inspect}" = inspect ]; then
    uname -v
    sysctl kern.hv_vmm_present
    csrutil status
    ls /Volumes
    exit 0
fi
[ "$1" = configure ] || exit 2
[ "$(id -u)" = 0 ] || { echo 'Recovery root shell required' >&2; exit 1; }
volume='/Volumes/Macintosh HD'
kc='/Volumes/RDXNU-VM/vm-boot.kc'
version="$volume/System/Library/CoreServices/SystemVersion.plist"
[ "$(/usr/libexec/PlistBuddy -c 'Print :ProductBuildVersion' "$version")" = 25F80 ] || exit 1
directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
actual=$("$directory/sha256" "$kc")
case "$actual" in
    4afc9861b7e074d28178cd6ca1bd0ff2b3837c3c7ef5a45dc357e05d2a520838) ;;
    *) echo 'REFUSED: VM collection digest mismatch' >&2; exit 1 ;;
esac
kmutil configure-boot --compress -v "$volume" -c "$kc"
# Recovery's unqualified common namespace refuses this variable on VMAPPLE.
bootargs='40a0ddd2-77f8-4392-b4a3-1e7304206516:boot-args'
nvram "$bootargs=-v rdxnu_linux_probe=1 -unsafe_kernel_text keepsyms=1 debug=0x108 serial=3"
nvram "$bootargs"
echo 'VM_CUSTOM_BOOT_CONFIGURED: reboot the guest, then verify its kernel UUID and probe'
