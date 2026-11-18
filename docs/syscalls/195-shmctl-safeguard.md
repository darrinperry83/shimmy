# ARM64 syscall 195: `shmctl` host IPC safeguard

The old handler accepted a guest numeric shared-memory ID as a Darwin SysV
ID. `IPC_RMID` could delete an unrelated host segment; `IPC_STAT` could expose
host metadata. Without a guest-private ID registry, neither operation can be
scoped safely to the Shimmy instance. This isolated change returns `ENOSYS`
for all `shmctl` commands before any host IPC call.

This is a narrow safety guard, not Linux-compatible shared memory. `shmget`
(194), `shmat` (196), and `shmdt` (197) now also refuse host IPC. Qt
`QSharedMemory` uses `IPC_STAT` and may stop working
under this guard. A complete design needs private guest IDs, permissions,
attachments, metadata, fork/exec sharing, and cleanup before the family can
be advertised as compatible.

`./build.sh cli` and `python3 tests/run-shmctl-refusal.py` check the build and
ARM64 ABI without making any host SysV IPC call. A private Arch clone reached
a fresh login prompt with D-Bus broker and launcher settled; its scoped guest
and exact clone were cleaned afterward. This boot smoke does not prove Qt
compatibility or shared-memory isolation. Linux's `shmctl` implementation is
in [ipc/shm.c](https://github.com/torvalds/linux/blob/v6.12/ipc/shm.c).
