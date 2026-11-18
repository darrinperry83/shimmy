# ARM64 `llistxattr` (#12) guest-memory boundary

The pathname is copied from guest memory before nofollow resolution. The host
lists the symlink inode's extended attributes into a temporary buffer with
`XATTR_NOFOLLOW`, and only returned bytes are copied to checked guest memory.
Zero-size requests remain length queries. The host buffer is bounded at 1 MiB,
so an unusually large list can return `ERANGE` despite a larger declared guest
capacity. A guest mapping changed during the call can still cause late
`EFAULT` after the host query.

`tests/run-llistxattr-memory.py` creates a disposable symlink and gives its
inode a different attribute name than its target. The raw ARM64 guest confirms
that #12 lists only the link name, while #11 follows and lists only the target
name. It also tests an executable-memory path, stack/static output, size
query, NUL-separated names, short-buffer `ERANGE` without mutation, invalid
and read-only output pointers, invalid path pointer, and a large declared
capacity with a small result.

The focused #12 and adjacent #11/#9 probes and CLI build passed. A private Arch
boot reached a fresh login prompt with D-Bus broker and launcher settled; the
exact guest and rootfs clone were cleaned.
