# `ioprio_get` (ARM64 syscall 31): raw self-priority subset

Linux returns the raw priority for `IOPRIO_WHO_PROCESS`, including zero when the current task has no explicitly set I/O priority. Group and user queries instead aggregate effective priorities across tasks. Shimmy's `ioprio_set` subset accepts only a default/unset self reset, so reporting zero for the current guest task is coherent. The previous handler reported zero for every selector and PID, including nonexistent tasks.

This branch accepts `IOPRIO_WHO_PROCESS` for `who=0` or the current guest PID, returning zero. Invalid selectors return `EINVAL`, and other PIDs return `ESRCH`. Group and user queries return `EOPNOTSUPP` until Shimmy has a guest task registry, effective-priority calculation, and I/O scheduler integration. This is a bounded query, not general I/O priority support.

Focused disposable ARM64 guest test: `python3 tests/run-ioprio-get.py --sysroot <Arch rootfs>`. It checks self by implicit and explicit PID, set/get consistency, invalid selectors, absent PIDs, and unsupported group/user queries.

Primary source: [Linux `block/ioprio.c`, `SYSCALL_DEFINE2(ioprio_get)`](https://github.com/torvalds/linux/blob/master/block/ioprio.c).
