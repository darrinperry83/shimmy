# `setxattr` (AArch64 syscall 5): checked guest inputs

The published path and name were raw guest pointers, and the value was
passed directly to the host xattr API. The `setxattr` branch now snapshots
both bounded strings and the value through checked guest memory before
resolving the file or changing an attribute. Values above Linux's 64 KiB
`XATTR_SIZE_MAX` are rejected. The existing CREATE and REPLACE translation
is preserved. The neighboring `lsetxattr` and `fsetxattr` paths are unchanged.

The disposable ARM64 probe checks NULL and unmapped pathname, name and
value pointers, oversized values, and successful create followed by replace.
It verifies the resulting attribute on a file inside a temporary guest root.
`./build.sh cli` passed. A separate private Arch boot reached a fresh login
prompt with the D-Bus broker and launcher settled; the harness stopped its
exact guest process.

This is a bounded input and ordinary-file subset. Full Linux xattr namespace,
permissions, symlink and concurrent mutation semantics remain unverified.
