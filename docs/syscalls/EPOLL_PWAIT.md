# `epoll_pwait` (ARM64 syscall 22)

Shimmy's network dispatcher used to drop the raw `sigmask` and `sigsetsize`
arguments, then wait on kqueue as though the mask had been applied. That can
break the atomic mask-and-wait contract on which signal-driven event loops
depend. The virtual signal mask is currently process-wide (`src/syscalls.c`),
so it cannot serve as a per-thread temporary mask around the kqueue wait.

This bounded implementation continues to handle a **NULL** mask through the
existing epoll wait path. For a supplied mask it returns `EINVAL` unless
`sigsetsize == 8` (the ARM64 kernel sigset size); with size 8 it returns
`EOPNOTSUPP` before consuming events. It does not claim full `epoll_pwait`
compatibility. `epoll_pwait2` already uses the same bounded mask policy.

The disposable ARM64 guest probe `python3 tests/run-epoll-pwait.py` checks
invalid size, valid-but-unsupported mask, unchanged event buffer on rejection,
NULL-mask readiness, and invalid `maxevents`. It does not exercise live
signal delivery or a multi-threaded event loop.

Linux primary reference: [`fs/eventpoll.c`, `do_epoll_pwait`](https://github.com/torvalds/linux/blob/v6.12/fs/eventpoll.c#L2326-L2355).
