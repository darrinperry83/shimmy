# `io_setup` (ARM64 syscall 0): isolated AIO-family prototype

This branch implements a bounded `io_setup(nr_events, ctxp)` context registry. It checks the guest's initial zero handle, rejects zero and oversized event counts, allocates a guest-visible anonymous completion ring through `gmem_mmap`, initializes the Linux ring header, and returns the ring address as the context handle. The per-process prototype accepts at most 32 contexts and 1,024 requested events. A disposable ARM64 guest probe checks pointer, zero-handle, count and ring-header behavior.

**Do not integrate this branch alone.** Syscalls `io_destroy`, `io_submit`, `io_cancel`, `io_getevents`, and `io_pgetevents` still return `ENOSYS`; a successful setup without those operations would leave an unusable context. The registry and ring layout are preparation for those branches, not a working AIO service. Before integration, implement context teardown and quota release, request submission with descriptor and guest-buffer lifetime, completion delivery, cancellation, wait/timeouts, fork/exec ownership, and guest unmapping behavior. Test the entire create–submit–complete–destroy lifecycle against Linux ARM64, including concurrent completions and ring wraparound.

Proof in this branch: `./build.sh cli` and `python3 tests/run-io-setup.py --sysroot <installed Arch root>` pass. The test root is disposable; the installed Arch root is only the read-only compiler sysroot. No live guest or GUI was changed.

Primary source: [Linux `fs/aio.c` `io_setup`](https://github.com/torvalds/linux/blob/master/fs/aio.c) and [Linux `aio_abi.h`](https://github.com/torvalds/linux/blob/master/include/uapi/linux/aio_abi.h).
