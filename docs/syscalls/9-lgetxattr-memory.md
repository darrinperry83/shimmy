# ARM64 #9 `lgetxattr`: checked symlink attribute read

The #9 route passed raw guest pathname, attribute-name and value pointers
through host resolution and `getxattr`. This bounded correction snapshots the
two guest strings, resolves without following the final symlink, stages up to
1 MiB of the host value and checked-copies only the returned bytes. A size
query remains valid with no output buffer; a large declared capacity can
retrieve a small value. The adjacent #8 `getxattr` and #10 `fgetxattr`
routes are unchanged.

`tests/run-lgetxattr-memory.py` places different xattrs on a link and its
target, then proves #9 reads the link inode while #8 follows the target. It
also checks executable guest pathname/name input, stack/static output,
size-only query, small-buffer `ERANGE` without output, bad/read-only
addresses, and a large declared capacity. The disposable ARM64 probe, CLI
build and private Arch login/D-Bus boot are regression gates, not full xattr
family parity.
