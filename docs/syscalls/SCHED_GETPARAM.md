# `sched_getparam` (ARM64 syscall 121): current-thread subset

Linux returns the requested thread's real-time scheduling priority in a `struct sched_param`. A null pointer or negative PID returns `EINVAL`; absent PID returns `ESRCH`, and a bad output address returns `EFAULT` after finding the target. [Linux source](https://github.com/torvalds/linux/blob/master/kernel/sched/syscalls.c#L954-L987).

Shimmy currently exposes only `SCHED_OTHER` for the calling guest thread, whose real-time priority is zero. PID 0, its guest TID, and the main thread's guest process PID return that value with checked guest copyout. Known other guest threads return `EOPNOTSUPP` until scheduler state can be queried per thread. Unknown IDs return `ESRCH`. Nice level is a separate setting and does not change `sched_priority` under `SCHED_OTHER`.

Run `./build.sh cli` and `python3 tests/run-sched-getparam.py`. The static ARM64 probe covers self, null and bad pointers, negative and absent targets, and verifies an absent target does not modify output. It does not certify real-time or other-thread scheduling.
