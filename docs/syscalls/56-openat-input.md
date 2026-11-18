# `openat` (AArch64 syscall 56): checked guest pathname

The published handler passed a raw guest pathname to the resolver. It now
copies a bounded NUL-terminated string through checked guest memory before
resolution, so a NULL or unmapped address returns `EFAULT`. The existing
open flags, synthetic device paths, and `openat2` delegation remain in place.

The disposable ARM64 probe checks NULL and unmapped paths, empty-path
`ENOENT`, regular-file read, and `O_CREAT` within a temporary guest root.
The original exploratory no-follow assertion used `0x20000`, which is not
`O_NOFOLLOW` on this guest ABI. The Arch AArch64 `asm/fcntl.h` defines
`O_NOFOLLOW` as octal `0100000` (`0x8000`). The corrected probe checks
`ELOOP` on a symlink and success on a regular file without changing runtime
code. `./build.sh cli` passed. A separate private Arch boot reached
a fresh login prompt with D-Bus broker and launcher settled; the harness
stopped its exact guest process.

This is a bounded path-input subset. Full flag, symlink, device, ownership,
permission and concurrency behavior remain unverified.
