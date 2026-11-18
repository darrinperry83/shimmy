# `fsync` (AArch64 syscall 82): reserved descriptor guard

The original handler sent any numeric guest descriptor to Darwin `fsync`.
Shimmy owns descriptors for services such as timerfd backing sockets. A
guest must not reach those internal host descriptors through a filesystem
sync request. The handler now returns `EBADF` for a negative or registered
shim-owned descriptor before calling Darwin.

The disposable ARM64 probe checks `-1`, a guest timerfd backed by a
shim-owned descriptor, and a regular file inside a temporary guest root.
The file succeeds and the internal descriptor is refused. The test does not
touch the installed rootfs or claim to prove power-failure durability.

Broader Linux `fsync` semantics, filesystem-specific durability, and the
host storage stack remain outside this focused descriptor safeguard.
