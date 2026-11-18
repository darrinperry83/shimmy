# `fchmod` (AArch64 syscall 52): reserved descriptor guard

The existing handler sent any numeric guest descriptor to Darwin `fchmod`.
Shimmy also holds descriptors for internal services, including the backing
descriptor of a guest timerfd. A guest must not change permissions on a
shim-owned descriptor by number. The handler now returns `EBADF` for negative
or registered shim-owned descriptors before calling Darwin.

The disposable ARM64 probe opens a file in a temporary guest root, changes
its mode to `0640`, confirms `EBADF` for `-1` and a guest timerfd, and checks
the temporary file's final mode from the host side. No installed rootfs or
file outside the disposable fixture has its permissions changed.

This guard does not establish complete Linux `fchmod` parity. Guest file
ownership, ACLs, and Linux permission semantics over Darwin-backed files
remain separate filesystem compatibility work.
