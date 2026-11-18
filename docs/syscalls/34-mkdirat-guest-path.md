# `mkdirat` (AArch64 syscall 34): checked guest pathname

The core handler previously sent a raw guest pathname to the shared
resolver, which reads `path[0]`. An unmapped nonzero guest address could
fault the shim instead of returning Linux `EFAULT`. The handler now copies
a bounded NUL-terminated path through checked guest memory before either
ordinary directory creation or the virtual cgroup path is considered.

The disposable ARM64 probe checks NULL and unmapped pointers, an empty path,
absolute and dirfd-relative directory creation, duplicate `EEXIST`, and
the resulting modes. It writes only into a temporary guest root.

This is an input safety and ordinary-filesystem subset. Guest ownership,
ACLs, concurrent path replacement and virtual cgroup creation are not
validated by this focused probe.
