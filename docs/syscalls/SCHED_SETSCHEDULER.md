# `sched_setscheduler` (#119)

The guest scheduler currently exposes `SCHED_OTHER` for its caller. This syscall
accepts `policy=0` with `sched_priority=0` for PID 0, the calling guest TID,
or the main guest process PID alias. That operation is an idempotent request;
it does not change Darwin scheduling state.

The Linux `struct sched_param` is copied through the checked guest-memory
interface. Null param and negative PID are `EINVAL`; inaccessible param is
`EFAULT`; absent PID is `ESRCH`. A known different guest thread is
`EOPNOTSUPP` because the host scheduler is process scoped. A nonzero priority
for `SCHED_OTHER` is `EINVAL`. Valid but unsupported policies, including
real-time and reset-on-fork requests, are `EOPNOTSUPP`; invalid policy numbers
are `EINVAL`. No call falsely reports an RT policy transition.

This bounded behavior follows Linux's public scheduler ABI and argument
validation in `kernel/sched/syscalls.c` (`do_sched_setscheduler`). It is
intentionally consistent with the separate `sched_getscheduler` (#120) branch,
which reports `SCHED_OTHER` for the caller.

Run the disposable raw-syscall test with `python3 tests/run-sched-setscheduler.py
--sysroot /path/to/arch/rootfs` after `./build.sh cli`. This does not test
changing another guest thread's scheduler or host OS scheduling policy.
