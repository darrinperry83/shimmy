# `sched_get_priority_max` (ARM64 syscall 125)

The Linux ABI returns 99 for `SCHED_FIFO` (1) and `SCHED_RR` (2), zero for
`SCHED_OTHER` (0), `SCHED_BATCH` (3), `SCHED_IDLE` (5), `SCHED_DEADLINE` (6),
and `SCHED_EXT` (7), and `EINVAL` for other policy numbers. The previous code
translated to Darwin policies and returned host ranges, which need not match.
Reporting the range does not imply Shimmy can set the corresponding policy.

Source: [Linux scheduler syscall implementation](https://github.com/torvalds/linux/blob/master/kernel/sched/syscalls.c).

Verification: `./build.sh cli` and `python3 tests/run-sched-priority-max.py`.
The probe runs in a disposable ARM64 guest rootfs.
