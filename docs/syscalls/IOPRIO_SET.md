# `ioprio_set` (ARM64 syscall 30): default-self subset

Shimmy previously returned success for every `ioprio_set` request without changing I/O scheduling. The supported subset now accepts `IOPRIO_WHO_PROCESS` for the current guest process with priority `IOPRIO_CLASS_NONE` and level 0, which resets an already-unset priority to its default. This is the only currently truthful success. Other valid priority classes and group/user fanout return `EOPNOTSUPP`; invalid class/level and selectors return `EINVAL`; an invalid process returns `ESRCH`. `ioprio_get` remains a separate legacy no-op to be corrected independently.

The subset cannot provide RT, best-effort level, or idle I/O scheduling. Those require a guest priority model and a host execution path that applies it. A future implementation should also preserve priority across threads and fork/exec and make `ioprio_get` reflect the stored priority.

Focused disposable ARM64 guest test: `python3 tests/run-ioprio-set.py --sysroot <Arch rootfs>`. It verifies default-self reset and unsupported/invalid responses. This does not validate host I/O scheduling.

Primary sources: [Linux `block/ioprio.c`](https://github.com/torvalds/linux/blob/master/block/ioprio.c) and [Linux UAPI `ioprio.h`](https://github.com/torvalds/linux/blob/master/include/uapi/linux/ioprio.h).
