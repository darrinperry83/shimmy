# ARM64 syscall 197: `shmdt` host mapping safeguard

The old `shmdt(addr)` path handed the guest address directly to Darwin.
Without a guest-owned attachment registry, a numeric address cannot establish
that the target is a Shimmy guest SysV shared-memory attachment. Detaching
an unrelated host mapping would violate the guest boundary. This isolated
change returns `ENOSYS` before any host `shmdt` call.

This is a narrow guard, not Linux-compatible shared memory. `shmget` (194),
`shmctl` (195), and `shmat` (196) now also refuse host IPC. A compatible
private backend must track segment IDs, attachments,
fork/exec inheritance, and detach/removal lifetime. Existing Qt
`QSharedMemory` flows may regress with an unavailable `shmdt`.

`./build.sh cli` and `python3 tests/run-shmdt-refusal.py` check the ARM64
return without creating or detaching host IPC. A private Arch clone reached a
fresh login prompt with D-Bus broker and launcher settled; its scoped guest
and exact clone were cleaned afterward. This boot smoke does not prove Qt
compatibility or shared-memory isolation. Linux's implementation is in
[ipc/shm.c](https://github.com/torvalds/linux/blob/v6.12/ipc/shm.c).
