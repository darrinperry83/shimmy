#!/bin/sh
# boot-getty.sh — boot the Arch guest to getty login prompts on tty1..tty6, then exec the
# desktop/command named by "$@". Invoke under the shim:
#
#   ./shim-metal-clear/shim_gui rootfs-arch \
#       /Users/.../shim-metal-clear/boot-getty.sh /usr/bin/Hyprland --config ...
#
# The shim boots ONE guest program; this script is that program. It forks the six agettys in the
# background and execs the real payload, so the guest reaches a login prompt on every text VT and
# still runs Hyprland/KDE on the graphics console. Each agetty runs under setsid so it becomes a
# session leader and can take its VT's controlling terminal (TIOCSCTTY), exactly as a real init's
# getty would. The shim re-execs itself for every guest exec, but the six pty pairs are shared by
# fd inheritance (vt.c), so every agetty writes to the SAME tty1..6 the main shim renders.
for vt in 1 2 3 4 5 6; do
  setsid /sbin/agetty --noclear tty$vt 38400 linux &
done
exec "$@"
