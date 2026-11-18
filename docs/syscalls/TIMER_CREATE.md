# `timer_create` (ARM64 syscall 107): bounded POSIX timer backend

Linux `timer_create(clockid, sigevent *, timer_t *)` allocates a process-owned timer ID and publishes it to the caller. A null event requests signal delivery by default; `SIGEV_NONE` suppresses notification, while signal and thread-ID modes need delivery through the guest signal system. A newly created timer is disarmed, but the ID must work with `timer_settime`, `timer_gettime`, `timer_getoverrun`, and `timer_delete`. Timers are shared among threads, removed on `fork`, and removed on `exec`.

Shimmy now supports a bounded process-owned timer family for `CLOCK_REALTIME` and `CLOCK_MONOTONIC` with explicit `SIGEV_NONE`. Creation, arming, querying, overrun readback, and deletion work together. Default signal delivery, `SIGEV_SIGNAL`, thread notification, and other clocks remain unsupported because the virtual signal path is incomplete.

## Supported subset and next work

The registry writes stable 32-bit IDs through checked guest memory, serializes concurrent creation, and clears in a fork child. Disposable static ARM64 tests cover pointer, clock, and notification errors, sequential IDs, fork isolation, arming, readback, overrun, and deletion. The CLI build and all five family tests pass together.

Add default and explicit signal notification only after the guest signal path can queue `SI_TIMER` with the correct timer ID and value. Further tests should cover concurrent deletion, exec cleanup, and a Linux ARM64 oracle before claiming broader compatibility.

Primary source: [Linux `kernel/time/posix-timers.c`, `do_timer_create` and `SYSCALL_DEFINE3(timer_create)`](https://github.com/torvalds/linux/blob/master/kernel/time/posix-timers.c).
