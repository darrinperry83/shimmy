# `timer_delete` (ARM64 syscall 111): staged SIGEV_NONE subset

On top of the isolated `timer_create` registry, `timer_delete(id)` removes a timer owned by the current guest process. A stale, unknown, or parent-process timer ID returns `EINVAL`. Deletion is serialized with creation and will release later arming state under the same registry lock.

Run `python3 tests/run-timer-delete.py` after `./build.sh cli` for disposable ARM64 create/delete/stale-ID coverage. Linux's [POSIX timer implementation](https://github.com/torvalds/linux/blob/master/kernel/time/posix-timers.c) uses per-process timer ID lookup and returns `EINVAL` for a missing timer. This stage remains isolated until `timer_settime`, `timer_gettime`, and `timer_getoverrun` are implemented and tested together; it is not a complete POSIX timer feature.
