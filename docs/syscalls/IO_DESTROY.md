# `io_destroy` (ARM64 syscall 1): AIO context teardown prototype

This branch adds teardown for the bounded `io_setup` context registry. The guest passes the 64-bit ring handle returned by `io_setup`; only a live context owned by the current host process is accepted. A valid destroy removes the registry entry and unmaps the ring. Zero, unknown, and repeated handles return `EINVAL`. The disposable ARM64 guest test covers context capacity and reuse after destruction.

Supported requests complete synchronously while holding the context lock, so `io_destroy` waits for that work before returning. A future asynchronous backend must drain in-flight requests explicitly. The bounded create, submit, retrieve, cancel, and destroy family passes combined disposable ARM64 tests.

Test: `python3 tests/run-io-destroy.py --sysroot <installed Arch root>`. Primary source: [Linux `fs/aio.c`, `SYSCALL_DEFINE1(io_destroy)`](https://github.com/torvalds/linux/blob/master/fs/aio.c).
