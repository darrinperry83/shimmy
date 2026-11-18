# `getresuid` (ARM64 148)

Shimmy's private `RDUID0` credential model tracks real, effective and saved
guest UIDs. `getresuid` now writes those three 32-bit values through checked
guest-memory copyout. Each pointer is required; null or invalid guest pointers
return `EFAULT`. The ARM64 probe checks the initial root tuple, a subsequent
`setresuid(1001,1002,1003)` tuple and invalid pointers.

Outside `RDUID0`, Darwin's `sysctl(KERN_PROC_PID, self)` process record supplies
the saved UID. The previous handler substituted the effective UID, which can
be wrong after a credential transition. The private ARM64 probe checks the
ordinary host-backed tuple against guest `getuid` and `geteuid`; no test
changes the host credentials.

This change does not grant host UID permissions or prove Linux filesystem
credential parity. Concurrent credential changes during the host query are
not made atomic with the other two values. The installed Arch boot path uses
`RDUID0`; an isolated private boot regression is still required at integration
time.
