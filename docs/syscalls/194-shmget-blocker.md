# `shmget` (ARM64 syscall 194): host IPC isolation blocker

The original `src/syscalls_ext.c` forwarded guest keys, sizes, and flags
directly to Darwin `shmget`. This bounded safeguard now returns `ENOSYS` for
all #194 requests, before any host IPC lookup or creation. The companion
`shmat` (196), `shmctl` (195), and `shmdt`
(197) routes likewise pass guest-visible segment IDs and addresses to the host.
The existing `shmctl(IPC_RMID)` path can remove a host segment whose numeric
ID the guest supplies. The guest and the user's Mac therefore share one SysV
IPC namespace, not just one physical memory implementation.

The former forwarding helped Qt `QSharedMemory` but never established Linux
namespace isolation or safe arbitrary-app compatibility. Direct companion
calls can still attach a same-user host segment, inspect or mutate its contents,
or remove it by guessed ID. Darwin ownership/permissions are those of the
host user rather than the virtual guest UID/GID. Host-attached pages are also
outside the shim's ordinary guest VMA accounting, so interactions with guest
memory-management calls need separate verification.

Refusing `shmget` can break Qt's `QSharedMemory`/SingleApplication path that
previously used host SysV IPC. It does not secure the companion calls: a guest
that supplies a host segment ID directly can still reach their host operations.
Those routes need separate safeguards before this family is isolated.

A coherent backend must issue guest-private keys and opaque guest IDs, authorize every ID use in
`shmat`/`shmctl`/`shmdt`, translate Linux permissions and metadata, track
attachments across fork/exec, and remove owned host segments on final cleanup.
It must never forward an unregistered guest ID to a Darwin IPC call. Rejecting
unsupported commands is preferable to success without the corresponding
memory or permission effect.

A future isolated test should run two shim socket groups and one unrelated host
process. Give all three the same SysV key and independently verify creation,
content isolation, `IPC_STAT`, attachment count, `IPC_RMID`, and teardown.
Include guessed host IDs and guest UID/GID changes. Until that test and a
private Arch boot gate pass, do not count #194–197 as isolated or fully
compatible. The focused #194 probe deliberately uses non-creating keys only;
it verifies the guest return without touching host shared-memory state. The
source-level unconditional refusal covers creating keys too, but no live
creation probe is run against the user's host IPC namespace.
