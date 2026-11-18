# `fdatasync` (AArch64 syscall 83): reserved descriptor guard

The handler previously sent every numeric guest descriptor to Darwin
`fsync`, including a shim-owned host descriptor if the number was supplied.
It now returns `EBADF` for a negative or registered shim-owned descriptor
before the host call. The existing Darwin `fsync` mapping is otherwise
unchanged.

The disposable ARM64 probe checks `-1`, a guest timerfd backed by a
shim-owned descriptor, and a regular file inside a temporary guest root.
The regular file succeeds and the internal descriptor is refused. Nothing
in the installed rootfs is modified.

This is a descriptor safeguard. A successful Darwin `fsync` call in the
probe does not prove Linux `fdatasync` power-failure or metadata durability
semantics across filesystems and storage hardware.
