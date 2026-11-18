# 153 `times`: bounded guest output

The effective `times` route is in `src/syscalls.c`; the similarly named
extended-table case is shadowed by that route. The core route obtains the
current shim process and waited-child CPU usage from Darwin `getrusage`,
converts microseconds to Linux's 100 Hz clock ticks, and returns ticks from
the host monotonic clock. A null output pointer is allowed. Non-null output
uses checked guest-memory copyout and returns `EFAULT` for invalid memory.

This is a per-shim process approximation. Darwin's resource accounting and
Linux's guest thread/process accounting need not coincide, and the monotonic
clock origin is not guaranteed to match Linux's boot-time origin. The call is
not a full CPU accounting or clock compatibility claim.

`python3 tests/run-times.py` exercises valid, null and invalid outputs and
monotonic tick ordering with a disposable static ARM64 guest.
