# `tee` (#77): virtual pipe buffer dependency

`src/virtual_pipe_core.c` is an **inactive** bounded pipe-buffer primitive.
It supports synthetic byte writes and reads, non-consuming `tee` to another
buffer, duplicate opens of the same backing object, and querying pending
bytes or free space. Each operation opens a fresh file description and uses a
stable two-pipe lock order; the small ARM64 host test proves that a forked,
re-execed child can duplicate bytes while the parent's source remains
unchanged, including a ring-buffer wrap and full-buffer case.

This is a dependency, **not a Linux pipe or syscall implementation**. Guest
`pipe2` still creates Darwin pipes in `src/net.c`; guest `read`, `write`,
`readv`, `writev`, `splice`, `vmsplice`, `dup`, `dup3`, `fcntl`, fork, exec, and
SCM_RIGHTS must all route through one virtual pipe identity before `tee`
could use this buffer. Poll/epoll wakeups, blocking waits, writer/reader
reference counts, EOF, SIGPIPE, descriptor flags and close semantics are
also missing. The `pending` and `space` queries are internal state, not a
pollable readiness notification. `tee` (#77) remains `ENOSYS`.

The prototype uses owner-only files in a private directory, but their bytes
are unencrypted and in-place writes are not crash-transactional. It must not
carry real guest pipe data yet. The test uses only disposable synthetic bytes
and removes its files. A production backend should use volatile shared
memory or another confidential, crash-safe design and prove cross-process
identity and wakeups. Do not enable #77 until an ARM64 **guest** test proves
non-consuming duplication with ordinary guest `pipe2`/read/write, dup,
fork/exec, nonblocking behavior and poll/epoll, followed by private Arch
login/D-Bus boot regression.

Focused dependency test:

```sh
clang -arch arm64 -O2 -Wall -Wextra -Werror -Isrc \
  tests/virtual-pipe-core.c src/virtual_pipe_core.c \
  -o /tmp/shimmy-virtual-pipe-core-test
/tmp/shimmy-virtual-pipe-core-test
```

Kernel behavior reference: [Linux `fs/splice.c` `do_tee`](https://github.com/torvalds/linux/blob/v6.12/fs/splice.c#L1857-L1921).
