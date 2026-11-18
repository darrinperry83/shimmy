# `fchown` (AArch64 syscall 55): reserved descriptor guard

The previous handler accepted any numeric guest descriptor. In virtual-root
mode, a shim-owned descriptor could acquire guest ownership metadata; in
host-identity mode, the same number could reach Darwin `fchown`. The handler
now returns `EBADF` for a negative or registered shim-owned descriptor before
either branch touches it.

The disposable ARM64 probe checks `-1` and a shim-backed guest timerfd in
both credential modes. A normal file remains usable: virtual-root mode writes
only its compatibility ownership metadata, while host-identity mode requests
no owner change. The host file UID and GID are checked unchanged.

This guard does not make virtual ownership an enforced Linux DAC boundary.
Guest ownership metadata, Linux ACLs, file capabilities, and the host's
actual permission checks remain separate compatibility work.
