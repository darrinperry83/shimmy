# `io_pgetevents` (ARM64 syscall 292): no-mask subset

Linux adds a sixth `const struct __aio_sigset *` argument to `io_getevents`. On ARM64 the structure holds a 64-bit guest pointer and 64-bit size. A NULL structure, or a structure with a NULL `sigmask`, leaves the signal mask unchanged and uses the same completion semantics as `io_getevents`. The mask size is ignored when `sigmask` is NULL.

Shimmy now supports that no-mask route for its bounded, synchronously completed regular-file AIO contexts. It copies the argument structure safely. A non-NULL mask with wrong size returns `EINVAL`; a bad mask address returns `EFAULT`; a valid mask returns `EOPNOTSUPP` because the AIO wait path cannot install and restore a per-thread temporary guest signal mask. It does not silently ignore the mask or consume completions on that rejection. A future implementation must integrate per-thread virtual signal masks and interruption/restart semantics.

Focused test: `python3 tests/run-io-pgetevents.py --sysroot <installed Arch root>`. This branch is based on the staged AIO chain and must not be merged alone. The full `io_setup`/`io_submit`/`io_getevents`/`io_pgetevents`/`io_cancel`/`io_destroy` lifecycle must pass together before integration.

Primary source: [Linux `fs/aio.c`, `SYSCALL_DEFINE6(io_pgetevents)`](https://github.com/torvalds/linux/blob/master/fs/aio.c) and [Linux `set_user_sigmask`](https://github.com/torvalds/linux/blob/master/kernel/signal.c).
