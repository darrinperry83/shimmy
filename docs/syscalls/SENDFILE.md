# `sendfile` (#71)

Shimmy already copied file bytes through `bounce_copy`, but it read and wrote
the optional guest offset with unchecked pointers. A bad pointer could crash
the shim, and a short transfer caused by a later write error could return a
positive byte count without advancing the offset.

The bounded implementation copies an eight-byte ARM64 offset through checked
guest memory, verifies the input is a readable regular file and the output is
writable, rejects append output and same-file copies, and transfers at most
1 MiB per call. A positive byte count advances the explicit offset by exactly
that count, including a short transfer. With a NULL offset, it reads from and
advances the input file's current position. The output position advances in
both cases. Bad guest pointers return `EFAULT`; invalid descriptors or access
modes return `EBADF`; invalid offset/output modes return `EINVAL`.

The offset cell is both input and output. A follow-up checks its guest
writability before any transfer, so a readable but executable read-only cell
returns `EFAULT` without changing the output or the input file position. The
ARM64 test compares output size and both positions around this case. A guest
mapping that changes after preflight can still cause a late copyout failure
after bytes have been transmitted.
The isolated private Arch boot reached a fresh login prompt with D-Bus broker
and launcher settled; its exact guest and clone were cleaned.

Linux [`fs/read_write.c`](https://codebrowser.dev/linux/linux/fs/read_write.c.html)
reads and writes the optional 64-bit offset around `do_sendfile`. The
disposable ARM64 test at `tests/run-sendfile.py` covers explicit and NULL
offsets, EOF short copy, content, bad pointers, append output, same-file
input/output, wrong access mode and closed descriptors. Run it after
`./build.sh cli` with `--sysroot /path/to/arch/rootfs`.

This is a buffered copy, not Linux's zero-copy path. Concurrent use of the
same file description, socket-specific behavior, and a short write followed
by error need further integration testing. No GUI or full boot claim follows
from the focused test.
