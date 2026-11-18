# `timer_getoverrun` (ARM64 syscall 109): staged SIGEV_NONE subset

For a process-owned timer created through the staged `timer_create` path with explicit `SIGEV_NONE`, `timer_getoverrun` returns zero. This is the Linux `it_overrun_last` value: it changes on timer signal delivery, which `SIGEV_NONE` never performs. Repeated interval expirations can advance a timer's next deadline when queried, but they do not create a delivered-signal overrun. Invalid, stale, and another process's timer IDs return `EINVAL`.

This branch builds on the isolated create/delete/set/get family chain and adds a disposable ARM64 test covering unarmed timers, multiple periodic expirations, and stale IDs. Run `./build.sh cli` and `python3 tests/run-timer-getoverrun.py`, then all earlier `run-timer-{create,delete,settime,gettime}.py` tests before integration. No host timer queue or notification mode is exposed. `SIGEV_SIGNAL`, `SIGEV_THREAD`, and `SIGEV_THREAD_ID` remain unsupported.

Linux primary source: [`kernel/time/posix-timers.c`, `timer_overrun_to_int`, signal delivery, and `SYSCALL_DEFINE1(timer_getoverrun)`](https://github.com/torvalds/linux/blob/master/kernel/time/posix-timers.c).
