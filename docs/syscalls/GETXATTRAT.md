# `getxattrat` (ARM64 syscall 464)

This implementation supports nonempty guest paths resolved by Shimmy's existing
`syscalls_resolve` path. It accepts `AT_SYMLINK_NOFOLLOW` and `AT_EMPTY_PATH`
with a nonempty path, and uses the existing Darwin xattr and errno translation.
It snapshots the guest path, name, and versioned 16-byte `xattr_args` structure;
the returned value is copied through the checked guest-memory path. Size-only
queries, `ERANGE`, zero-extended future structures, and the 64 KiB Linux value
limit are handled.

`AT_EMPTY_PATH` with a null or empty path, which selects an fd in Linux, returns
`EOPNOTSUPP` here. Sending an unvalidated guest fd directly to Darwin's
`fgetxattr` could read an unrelated host object. That route needs guest-fd
ownership and descriptor translation first. This also inherits the existing
cross-filesystem differences between Linux and Darwin xattr namespaces and
permissions; it does not claim full Linux filesystem semantics.

The ABI and size handling follow Linux's `struct xattr_args` in
`include/uapi/linux/xattr.h` and `SYSCALL_DEFINE6(getxattrat)` / `do_getxattr`
in [`fs/xattr.c`](https://github.com/torvalds/linux/blob/master/fs/xattr.c).
The disposable ARM64 check is `tests/run-getxattrat.py`.
