# `removexattrat` (ARM64 syscall 466)

The pathname form removes an extended attribute from a file inside the guest
root. It accepts absolute guest paths and paths relative to a guest directory
descriptor. `AT_SYMLINK_NOFOLLOW` is forwarded to the host xattr operation;
unknown flags return `EINVAL`. Guest pathname and attribute-name pointers are
copied through checked guest memory access before invoking the host operation.

The `AT_EMPTY_PATH` descriptor form returns `EOPNOTSUPP` because passing an
unverified descriptor to `fremovexattr` could mutate a shim-private host file.
This is a bounded subset, not complete Linux xattr permission and namespace
compatibility. In particular, namespace mapping, synthetic filesystems,
guest ownership, and pathname race behavior are unverified.

Linux ABI reference: [`fs/xattr.c`, `SYSCALL_DEFINE4(removexattrat)`](https://github.com/torvalds/linux/blob/master/fs/xattr.c#L1010-L1037).

Verification: `./build.sh cli` and
`python3 tests/run-removexattrat.py --sysroot <installed-Arch-root>`.
