# `statfs` (AArch64 syscall 43): checked guest pathname

The existing output conversion already copied the Linux `statfs` structure
through checked guest memory. Its input path still went directly to the
resolver as a host C string. The handler now snapshots a bounded pathname
through checked guest memory before resolution, returning `EFAULT` for a
NULL or unmapped address.

The expanded disposable ARM64 probe checks bad and empty input paths, a
missing path, valid `/tmp` output, invalid output pointers, and the output
boundary canary. `./build.sh cli` passed. A separate private Arch boot
reached a fresh login prompt with D-Bus broker and launcher settled; the
harness stopped its exact guest process.

This is a bounded input and output subset; it does not establish full
Linux filesystem identity or mount-namespace semantics.
