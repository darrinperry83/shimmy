# `timer_settime` (ARM64 syscall 110): staged `SIGEV_NONE` subset

This branch extends the isolated POSIX timer registry created by syscall 107 and deletion in syscall 111. It accepts valid 64-bit Linux `itimerspec` values for explicit `SIGEV_NONE` timers on guest `CLOCK_REALTIME` and `CLOCK_MONOTONIC`. Relative and `TIMER_ABSTIME` deadlines, disarming, interval storage, periodic expiry forwarding on readback, and the previous setting output are implemented under the registry lock. Shimmy's guest monotonic clock maps to Darwin `CLOCK_UPTIME_RAW`; the timer uses that same clock so absolute guest deadlines have the same epoch.

Only flag 0 or `TIMER_ABSTIME` is supported. Other flag bits return `EOPNOTSUPP`. Bad nanoseconds or negative times return `EINVAL`; a bad new-setting pointer returns `EFAULT`; an unknown timer ID returns `EINVAL`. Linux copies the old setting after changing a timer: this subset likewise returns `EFAULT` for a bad old-setting pointer after installing the new deadline. `SIGEV_NONE` never delivers a signal or enqueues a host timer; remaining time is computed when queried. Other notification modes were excluded by timer creation.

Focused disposable ARM64 test: `python3 tests/run-timer-settime.py`. It covers relative deadlines, realtime and monotonic absolute deadlines, disarm, periodic forwarding, huge-duration saturation, old-value copyout, and validation errors. Run it with the create and delete tests after `./build.sh cli`.

**Do not merge or publish this branch alone.** `timer_gettime` and `timer_getoverrun` remain `ENOSYS`. The full staged family must pass combined tests before it can be called usable.

Primary source: [Linux `kernel/time/posix-timers.c`, `timer_settime` and `common_timer_get`](https://codebrowser.dev/linux/linux/kernel/time/posix-timers.c.html).
