# `openat` (AArch64 syscall 56): checked guest pathname

The published handler passed a raw guest pathname to the resolver. It now
copies a bounded NUL-terminated string through checked guest memory before
resolution, so a NULL or unmapped address returns `EFAULT`. The existing
open flags, synthetic device paths, and `openat2` delegation remain in place.

The disposable ARM64 probe checks NULL and unmapped paths, empty-path
`ENOENT`, regular-file read, and `O_CREAT` within a temporary guest root.
`./build.sh cli` passed. A no-follow assertion failed in an exploratory
candidate probe and was removed because it does not test the path-input
change; no no-follow claim is made here. A separate private Arch boot reached
a fresh login prompt with D-Bus broker and launcher settled; the harness
stopped its exact guest process.

This is a bounded path-input subset. Full flag, symlink, device, ownership,
permission and concurrency behavior remain unverified.
