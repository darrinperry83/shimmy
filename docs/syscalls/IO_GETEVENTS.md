# `io_getevents` (ARM64 syscall 4): bounded regular-file AIO subset

This dependent branch extends staged `io_setup` → `io_destroy` → `io_submit` with completion retrieval. It supports the accepted regular-file PREAD/PWRITE requests from `io_submit`, returning their `io_event` records in FIFO order. `min_nr`/`nr` validation, invalid contexts, checked output addresses, and relative timespec validation are implemented. A zero timeout returns available events; another guest thread can submit while a caller waits on the context's condition variable.

The shim mirrors completed events in the guest AIO ring. `io_getevents` consumes the same host queue and advances the ring head. If userspace advanced the mmap ring head directly before the syscall, the host queue reconciles that change so events are not delivered twice. Unsupported multi-consumer races and signal interruption remain to be validated. `min_nr` above the context's bounded capacity returns `EOPNOTSUPP` because incremental draining across future submissions is not implemented. The backend has no asynchronous host worker: accepted PREAD/PWRITE complete within `io_submit`, though another guest thread can submit during a wait.

Run `python3 tests/run-io-getevents.py --sysroot <installed Arch root>` for the disposable ARM64 guest test. It covers create → submit → getevents → destroy, empty and short timeouts, pointer and count errors, no duplicate delivery, and direct-ring consumption. Also run the earlier setup, destroy, and submit tests together after each merge.

The full bounded AIO family now passes its combined disposable ARM64 tests. This result does not establish full Linux AIO compatibility.

Primary source: [Linux `fs/aio.c`, `aio_read_events_ring` and `io_getevents`](https://github.com/torvalds/linux/blob/master/fs/aio.c).
