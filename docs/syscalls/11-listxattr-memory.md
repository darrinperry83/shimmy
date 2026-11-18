# ARM64 `listxattr` (#11) guest-memory boundary

The pathname is copied from guest memory before resolution. The host writes
the NUL-separated name list to a temporary buffer; only the returned bytes are
copied to checked guest memory. A zero-size request remains a length query.
The host buffer is capped at 1 MiB, so larger lists can return `ERANGE` even
if the guest declared a larger capacity. This is a bounded compatibility
subset; a guest mapping changed during the call can still make copyout fail
after the host query.

`tests/run-listxattr-memory.py` builds a disposable static ARM64 guest and
creates a file with `user.shimmy-test`. The probe covers a path in executable
guest memory, an ordinary symlink to that path, size query, NUL-separated
names, stack/static outputs, `ERANGE` without output mutation, invalid and
read-only output pointers, invalid path pointer, and a large declared capacity
with a small actual result.

The focused probe and `./build.sh cli` passed. A private Arch boot reached a
fresh login prompt with D-Bus broker and launcher settled; the exact guest and
rootfs clone were cleaned. Other xattr variants remain separate work.
