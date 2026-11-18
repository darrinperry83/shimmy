# `io_destroy` (ARM64 syscall 1): AIO context teardown prototype

This branch adds teardown for the bounded `io_setup` context registry. The guest passes the 64-bit ring handle returned by `io_setup`; only a live context owned by the current host process is accepted. A valid destroy removes the registry entry and unmaps the ring. Zero, unknown, and repeated handles return `EINVAL`. The disposable ARM64 guest test covers context capacity and reuse after destruction.

This is **not ready for integration on its own**. `io_submit`, `io_cancel`, and `io_getevents` remain `ENOSYS`. Once requests exist, `io_destroy` must stop new submissions and wait for every in-flight operation before returning, as Linux does. The current implementation is safe only because this prototype has no request submission path. Merge the AIO family only after a combined create, submit, complete, destroy test passes.

Test: `python3 tests/run-io-destroy.py --sysroot <installed Arch root>`. Primary source: [Linux `fs/aio.c`, `SYSCALL_DEFINE1(io_destroy)`](https://github.com/torvalds/linux/blob/master/fs/aio.c).
