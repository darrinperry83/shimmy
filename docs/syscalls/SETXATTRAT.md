# `setxattrat` (ARM64 syscall 463)

Shimmy supports a bounded pathname form of Linux `setxattrat`: absolute paths and paths relative to a guest directory fd, resolved through the existing guest-root path resolver. It copies the versioned 16-byte `xattr_args` and its value from guest memory before mutating a file. Linux `XATTR_CREATE` and `XATTR_REPLACE` map to their Darwin counterparts. `AT_SYMLINK_NOFOLLOW` is translated to the host xattr option.

Validation includes the argument size and zero extension, the 64 KiB Linux value limit, xattr flags, pathname flags, names, and guest pointers. Unsupported fd/`AT_EMPTY_PATH` requests return `EOPNOTSUPP` instead of writing through an unproven host descriptor.

This does not establish full Linux xattr namespace, ACL, security-label, symlink, permission, or race semantics on APFS. The older `setxattr`/`lsetxattr`/`fsetxattr` cases have separate behavior and were not changed here.

Proof: `./build.sh cli` and `python3 tests/run-setxattrat.py --sysroot <installed-Arch-root>` pass using a disposable ARM64 guest root. The guest test covers create, replace, dirfd, version extension, bounds, bad pointers, invalid flags, and unchanged contents after rejected calls. It does not use the live desktop or installed rootfs for writes.

ABI reference: [Linux fs/xattr.c `SYSCALL_DEFINE6(setxattrat)`](https://github.com/torvalds/linux/blob/master/fs/xattr.c#L696-L717).
