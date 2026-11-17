# Unix socket O_PATH bridge

Darwin cannot open a Unix socket pathname. Shimmy now handles a guest `O_PATH`
open by resolving the pathname in the guest socket namespace, checking its
`(st_dev, st_ino)`, and making a private hard link to that same socket inode.
The guest receives an anonymous, metadata-only fd. An extended attribute on
its backing object records the socket identity and private link, so the
reference survives fork, re-exec and `SCM_RIGHTS` transfer. The receiver can
`fstat`, `fstatat(AT_EMPTY_PATH)` or `statx(AT_EMPTY_PATH)` the socket and
connect through `/proc/self/fd/N` after the original pathname is removed.
The real listener fd is never handed out as an O_PATH reference.

The hard link is validated against the original inode after creation. Each
reference gets a unique private link, so rebinding the same pathname cannot
retarget an existing reference. The link remains until its private socket
namespace is removed; the bridge does not yet reclaim individual links on
last close. The backing fd is metadata-only but is not a complete Linux
O_PATH implementation for every operation: for example, Darwin returns EOF
on `read` rather than Linux's `EBADF`. Ordinary file O_PATH behavior is
unchanged.

Verified with `sh tests/run-net-tests.sh` (204 checks), CLI and GUI builds,
and `tests/guest_opath_socket.c` run as a static Linux guest in a disposable
minimal root. The guest test covers a different process opening the socket,
stat/fstatat/statx, `SCM_RIGHTS`, re-exec, unlink and connect through the
reference. These tests do not establish that KWin's separate black-frame
stall is fixed. The disposable full Plasma clone used for the on-screen
attempt did not reach KWin because its D-Bus runtime directory ownership was
wrong; that is a separate session setup issue.
