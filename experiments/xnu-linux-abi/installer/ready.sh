#!/usr/bin/bash
set -euo pipefail
[[ $(cat /proc/1/comm) == systemd ]]
systemctl is-active --quiet sysinit.target
systemctl is-active --quiet basic.target
systemctl is-active --quiet multi-user.target
systemctl is-active --quiet console-getty.service
systemctl is-active --quiet systemd-networkd.service
systemctl is-active --quiet systemd-resolved.service
for tool in bash pacman pacstrap arch-chroot genfstab; do
    command -v "$tool" >/dev/null
done
pacman -Q base filesystem bash pacman systemd arch-install-scripts
bash --noprofile --norc -c '[[ $((2+2)) == 4 ]]'
pacstrap -h >/dev/null
# Keep the complete failure list: a reached target alone can hide failed jobs.
failed=$(systemctl --failed --plain --no-legend --no-pager)
if [[ -n "$failed" ]]; then
    printf '%s\n' "$failed" >&2
    exit 1
fi
printf 'RDXNU_ARCH_INSTALLER_INIT_READY\n'
