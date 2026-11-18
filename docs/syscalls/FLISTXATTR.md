# `flistxattr` (#13)

The previous handler gave Darwin a guest output pointer directly. Guest ELF
or executable memory may not be host writable, and an invalid pointer did not
go through Shimmy's checked guest-memory boundary.

The bounded ARM64 path reads the NUL-separated list into host memory, then
copies exactly the returned bytes into guest memory. A size of zero remains a
length query without output. The host staging buffer is capped at 1 MiB;
valid Linux xattr names and values ordinarily fit beneath that cap, and a
caller may advertise a larger buffer while still receiving a short list. A
host list over the cap can return `ERANGE` even if the guest asked for more.
The handler preserves the host's `ERANGE` for a too-small buffer and does not
copy a partial list on that error.

The disposable ARM64 probe creates one file with a host xattr. It checks the
size query, NUL-separated name, stack and static output, small-buffer
`ERANGE`, invalid and executable read-only destinations, a large declared
capacity with a short list, and invalid fd. The adjacent path variants
`listxattr` and `llistxattr` still need their own guest-memory fixes.

The isolated CLI build, ARM64 probe and private Arch LOGIN/D-Bus boot passed;
the boot runner stopped its exact guest and the disposable clone was cleaned.
