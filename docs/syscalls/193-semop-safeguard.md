# `semop` (AArch64 syscall 193): host semaphore ID safeguard

The previous handler passed the guest's numeric `semid` and operation array
directly to Darwin `semop`. Darwin SysV semaphore sets are host-global. A
guessed ID could operate on, wait on, or consume a semaphore owned by another
macOS process. The guest pointer was not checked before the host call.

This safeguard returns `ENOSYS` before either the host IPC call or the guest
array access. The focused ARM64 probe covers plausible raw IDs and an invalid
array pointer; it creates no host IPC object and invokes no host semaphore.

This is **not** Linux-compatible SysV semaphore support. `semget`, `semctl`,
`semtimedop`, and `semop` need one instance-scoped key/ID/permission model,
checked guest arrays, waiting and timeout semantics, and lifecycle cleanup.
The remaining ID-taking companion calls are separate risks until safeguarded.
