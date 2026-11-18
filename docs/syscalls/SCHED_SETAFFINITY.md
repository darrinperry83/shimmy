# `sched_setaffinity` (#122)

Shimmy does not have a Linux CPU pinning backend. The bounded operation accepts
an affinity mask containing every CPU in Shimmy's virtual online topology and
leaves host scheduling unrestricted. A mask excluding any online CPU is
`EOPNOTSUPP`, rather than a false success. The virtual count is the host online
count capped at 32, matching the guest `/sys` and `/proc/self/status` topology.
The companion `sched_getaffinity` (#123) change uses the same count.

PID 0, the calling guest TID and the main guest PID alias name the caller.
A known different guest TID is unsupported; an unknown TID is `ESRCH`. The
up-to-128-byte mask is copied through checked guest memory. A null or invalid
mask is `EFAULT`, sizes below eight bytes are `EINVAL`, and larger masks are
outside this bounded implementation (`EOPNOTSUPP`).

The Linux [`sched_setaffinity(2)` manual](https://man7.org/linux/man-pages/man2/sched_setaffinity.2.html)
describes its per-thread mask, PID 0, `cpusetsize`, and intersection with online
CPUs. Run `python3 tests/run-sched-setaffinity.py --sysroot /path/to/arch/rootfs`
after `./build.sh cli` for the disposable raw ARM64 syscall probe. This does not
claim actual CPU pinning or a performance change.
