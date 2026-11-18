# `sched_setparam` (ARM64 syscall 118)

This bounded implementation recognizes PID 0, the caller's guest TID, and
the main thread's PID alias. It copies `struct sched_param` from guest memory
and accepts priority zero under Shimmy's current `SCHED_OTHER` policy.
Invalid PID/null parameter return `EINVAL`; inaccessible guest parameter
returns `EFAULT`; unknown PID returns `ESRCH`; another known guest TID returns
`EOPNOTSUPP` because Shimmy cannot yet adjust that thread's policy safely.
Nonzero priority returns `EINVAL` for `SCHED_OTHER`. No host scheduler change
is made for the zero-priority case.

Linux's [`sched_setparam` implementation](https://github.com/torvalds/linux/blob/master/kernel/sched/syscalls.c)
uses the existing policy and reads a `sched_param` containing the RT priority.
The local probe compiles an ARM64 guest into a disposable rootfs and checks
the accepted and error paths: `python3 tests/run-sched-setparam.py`.
