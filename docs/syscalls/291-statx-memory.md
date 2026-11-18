# ARM64 `statx` (#291) guest-memory boundary

The pathname is copied from guest memory before ordinary resolution or
synthetic-path checks. A NULL pathname remains valid only with `AT_EMPTY_PATH`.
The 256-byte Linux `statx` result is first built in host memory, then copied
through checked guest output. A failed path lookup leaves the guest result
untouched. The existing #258 alias uses this same handler and receives the
same pointer handling.

`tests/run-statx-memory.py` runs a disposable raw ARM64 guest against an
ordinary file. The guest tests a pathname in executable memory, file type and
size, invalid and absent paths without output mutation, invalid and read-only
output pointers including a result crossing into a protected page, empty and
NULL names with `AT_EMPTY_PATH`, and the existing
synthetic `/sys/fs/cgroup` directory result.

The focused probe and CLI build passed. A private Arch boot reached a fresh
login prompt with D-Bus broker and launcher settled; the exact guest and
rootfs clone were cleaned. This does not establish full Linux `statx` mask, flag,
timestamp, or mount-ID compatibility; it closes the direct guest-pointer
dereference in this dispatch path.
