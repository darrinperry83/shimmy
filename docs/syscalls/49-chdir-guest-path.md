# `chdir` (AArch64 syscall 49): checked guest pathname

The core handler previously passed the raw guest pathname pointer into the
shared resolver, which inspects `path[0]`. An unmapped nonzero guest address
could fault the shim instead of returning Linux `EFAULT`. The handler now
copies a bounded NUL-terminated path through checked guest memory before
resolution or a guest cwd update.

The disposable ARM64 probe checks NULL and unmapped pointers, an empty path,
an absolute directory, a relative subdirectory, `getcwd`, and a relative
file open after the cwd change. Failed path requests leave the guest cwd
unchanged. No installed rootfs state is changed.

This addresses guest path safety and a focused cwd behavior. Concurrent
filesystem changes, permission mapping, and all symlink edge cases remain
outside this probe.
