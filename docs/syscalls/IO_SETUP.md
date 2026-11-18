# `io_setup` (ARM64 syscall 0): bounded AIO context

This branch implements a bounded `io_setup(nr_events, ctxp)` context registry. It checks the guest's initial zero handle, rejects zero and oversized event counts, allocates a guest-visible anonymous completion ring through `gmem_mmap`, initializes the Linux ring header, and returns the ring address as the context handle. The per-process prototype accepts at most 32 contexts and 1,024 requested events. A disposable ARM64 guest probe checks pointer, zero-handle, count and ring-header behavior.

The bounded AIO family now supports setup, teardown, synchronous regular-file PREAD/PWRITE submission, completion retrieval, and the completed-request cancellation result. It does not provide general asynchronous I/O, active-request cancellation, or non-NULL temporary signal masks. Concurrent direct-ring readers and syscall consumers remain unverified.

Proof in this branch: `./build.sh cli` and `python3 tests/run-io-setup.py --sysroot <installed Arch root>` pass. The test root is disposable; the installed Arch root is only the read-only compiler sysroot. No live guest or GUI was changed.

Primary source: [Linux `fs/aio.c` `io_setup`](https://github.com/torvalds/linux/blob/master/fs/aio.c) and [Linux `aio_abi.h`](https://github.com/torvalds/linux/blob/master/include/uapi/linux/aio_abi.h).
