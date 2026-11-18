# ARM64 syscall 190: `semget` isolation blocker

The current extended dispatcher passes the guest key, semaphore count and
flags directly to Darwin `semget()` (`src/syscalls_ext.c`). Its companion
`semctl`, `semop` and `semtimedop` cases accept the resulting raw host ID.
`semctl(IPC_RMID)` can remove that host object. This is a real host-global
SysV IPC namespace, not a private Linux guest namespace. A same-UID Mac
process or another Shimmy instance can collide with a guest key or ID; the
guest can also name an object it did not create. Passing the same pointer
layout to Darwin does not solve key ownership, identifier authority, or
cleanup after a shim dies.

There is no safe `semget`-only runtime patch: key translation without a
private ID registry leaves companion calls able to address arbitrary host
IDs, and translating only returned IDs would strand `semctl` and `semop`.
The existing host-forwarding path is retained for now because Qt applications
have used it, but it must not be counted as isolated Linux syscall parity.
The present `semtimedop` case also calls blocking host `semop` for a finite
timeout, so its deadline contract is separately unimplemented.

The next implementation needs a guest-owned semaphore backend or a private
broker shared by all four calls. It must bind guest keys and IDs to one owner
group, enforce permissions, translate every companion operation, share state
across guest fork/exec, and clean objects after `IPC_RMID` and abnormal exit.
Tests should run two isolated Shimmy guest groups plus an unrelated host
process, verify key and guessed-ID collision refusal, operation/permission
semantics, timed wait expiry, and crash cleanup. Then run the private Arch
login/D-Bus boot gate and an actual Qt semaphore consumer.

This branch changes documentation only; it makes no runtime compatibility
claim and does not run a boot test.
