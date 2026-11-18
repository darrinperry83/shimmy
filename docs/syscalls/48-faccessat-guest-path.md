# `faccessat` (AArch64 syscall 48): checked guest pathname

The core handler previously passed the raw guest pathname pointer into the
shared resolver, which reads `path[0]`. An unmapped nonzero guest address
could fault the shim instead of returning Linux `EFAULT`. The handler now
copies a bounded NUL-terminated pathname through checked guest memory
before resolution.

The disposable ARM64 probe checks NULL/unmapped pointers, empty and missing
paths, file existence/readability, a path relative to a directory fd, and
that x3 is ignored. Syscall 48 has three Linux arguments; flags belong to
`faccessat2` (#439). No installed rootfs state is modified.

This is input safety and focused access behavior, not a complete guest
permission model. Darwin-backed ownership, ACLs, and credential mapping
remain separate compatibility work.
