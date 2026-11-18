# `sched_getscheduler` (ARM64 syscall 120): current-thread subset

Linux takes a thread PID, returning `EINVAL` for negative IDs, `ESRCH` when no thread exists, and that thread's actual scheduling policy on success. Shimmy previously returned `SCHED_OTHER` (0) for every ID, including nonexistent processes. This branch returns 0 only for the calling guest thread (PID 0, its guest TID, or the main thread's guest process PID). A known but different guest thread returns `EOPNOTSUPP` until a per-thread scheduler state model exists; an absent ID returns `ESRCH`.

This is a bounded identity correction. **It is not fully coherent until `sched_setscheduler` (#119) is fixed:** that call currently reports success for nondefault policies without applying them, so a subsequent get may still read 0. Keep this branch paired with a separate #119 correction in integration. `sched_setattr` (#274) already limits successful writes to `SCHED_OTHER` and an actually applied nice value.

Run `./build.sh cli` and `python3 tests/run-sched-getscheduler.py`; the disposable ARM64 guest test covers PID 0, current TID, main PID, negative PID and absent PID. It does not certify policies for other threads or real-time scheduling.

Primary source: [Linux `kernel/sched/syscalls.c`, `SYSCALL_DEFINE1(sched_getscheduler)`](https://github.com/torvalds/linux/blob/master/kernel/sched/syscalls.c).
