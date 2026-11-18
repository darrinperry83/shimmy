# `renameat` (AArch64 syscall 38): checked guest pathnames

The core handler previously handed both raw guest pointers to the path
resolver. An unmapped pointer could fault the shim instead of returning
Linux `EFAULT`. It now copies both bounded NUL-terminated strings through
checked guest memory before resolving either one or changing the filesystem.

The disposable ARM64 probe checks NULL and unmapped old/new pointers, an
absolute rename, a dirfd-relative rename, and resulting source and target
files. It writes only inside a temporary guest root. `./build.sh cli` passed.
The separate private Arch boot reached a fresh login prompt with the D-Bus
broker and launcher settled, and the harness stopped its exact guest process.

This establishes an input-safety and ordinary-file subset. Cross-filesystem
behavior, guest permissions, concurrent path replacement, and virtual cgroup
rename semantics are not established by this probe.
