# `shmget` (ARM64 syscall 194): host IPC isolation blocker

`src/syscalls_ext.c` currently forwards guest keys, sizes, and flags directly
to Darwin `shmget`. The companion `shmat` (196), `shmctl` (195), and `shmdt`
(197) routes likewise pass guest-visible segment IDs and addresses to the host.
The existing `shmctl(IPC_RMID)` path can remove a host segment whose numeric
ID the guest supplies. The guest and the user's Mac therefore share one SysV
IPC namespace, not just one physical memory implementation.

This is useful for the existing Qt `QSharedMemory` path but does not establish
Linux namespace isolation or safe arbitrary-app compatibility. A guest can
collide with a host key, attach a same-user host segment, inspect or mutate its
contents, or remove it by ID. Darwin ownership/permissions are those of the
host user rather than the virtual guest UID/GID. Host-attached pages are also
outside the shim's ordinary guest VMA accounting, so interactions with guest
memory-management calls need separate verification. A crash before `IPC_RMID`
can leave a persistent host segment.

Changing `shmget` alone would break the companion calls. A coherent backend
must issue guest-private keys and opaque guest IDs, authorize every ID use in
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
compatible. This audit changes no runtime behavior.
