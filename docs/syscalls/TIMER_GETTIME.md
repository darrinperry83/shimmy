# `timer_gettime` (ARM64 syscall 108): staged SIGEV_NONE subset

This branch builds on the isolated `timer_create`/`timer_delete`/`timer_settime` chain. It returns the current interval and remaining time for process-owned `CLOCK_MONOTONIC` and `CLOCK_REALTIME` timers created with explicit `SIGEV_NONE`. Disarmed and expired one-shot timers return zero remaining time; periodic timers advance their next deadline when queried. Invalid/stale IDs return `EINVAL`; an invalid output address returns `EFAULT` after ID lookup. The output is the Linux ARM64 32-byte `__kernel_itimerspec`.

The branch also aligns staged `timer_settime` with Linux: disarming clears the interval, and a relative `CLOCK_REALTIME` timer uses a monotonic timebase so later wall-clock changes cannot move its deadline. The old setting is read with its previous timebase before a mode switch. Shimmy's guest `CLOCK_MONOTONIC` maps to Darwin `CLOCK_UPTIME_RAW`.

Run `./build.sh cli` and `python3 tests/run-timer-gettime.py` to exercise unarmed, armed, expired, periodic, disarm, realtime relative/absolute, mode transition, bad pointers and IDs. This family is still **unmerged** until `timer_getoverrun` is implemented and the combined create/set/get/overrun/delete test passes. `SIGEV_SIGNAL` and thread notification modes are unsupported.

Primary source: [Linux `kernel/time/posix-timers.c`](https://github.com/torvalds/linux/blob/master/kernel/time/posix-timers.c), especially `common_timer_get` and `SYSCALL_DEFINE2(timer_gettime)`.
