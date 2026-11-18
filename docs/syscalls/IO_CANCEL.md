# `io_cancel` (ARM64 syscall 3): completed-request subset

The staged native-AIO backend accepts only regular-file PREAD/PWRITE requests that finish synchronously inside `io_submit`. It has no outstanding request by the time another guest thread can call `io_cancel`. Linux first reads the `iocb`'s `aio_key`; a bad pointer returns `EFAULT`, and a nonzero key returns `EINVAL`. If no matching request is active, Linux returns `EINVAL`. The shim now follows that path without consuming an already queued completion. The `result` pointer is unused on this failure path.

This is a bounded semantic result, not a general cancellation implementation. A future asynchronous backend needs an active-request registry, cancellation callbacks, race handling with completion, and `io_destroy` draining. The create, submit, retrieve, cancel and destroy family now passes combined disposable ARM64 tests; `io_pgetevents` supports the no-mask path.

Focused guest test: `python3 tests/run-io-cancel.py --sysroot <installed Arch root>`. It checks bad `iocb` addresses, invalid keys, unknown context, completed requests, and that a failed cancellation leaves the completion retrievable.

Primary source: [Linux `fs/aio.c`, `SYSCALL_DEFINE3(io_cancel)`](https://codebrowser.dev/linux/linux/fs/aio.c.html#2172).
