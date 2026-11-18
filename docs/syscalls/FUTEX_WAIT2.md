# `futex_wait` (AArch64 syscall 455)

This branch implements a bounded private U32 form using Shimmy's existing
`FUTEX_WAIT_BITSET` queue. The syscall uses the futex2 six-argument order:
`uaddr, expected_value, wake_mask, flags, absolute_timeout, clockid`.

Supported behavior:

- A 4-byte aligned, mapped U32 word with `FUTEX2_SIZE_U32 | FUTEX2_PRIVATE`.
- Expected-value comparison returns `EAGAIN` without parking on mismatch.
- Masked wake from syscall 454 wakes one parked waiter.
- Absolute `CLOCK_MONOTONIC` or `CLOCK_REALTIME` timeout, including expiry.
- Invalid masks, flags, sizes, addresses, and timeout fields return errors.

Shared futexes and NUMA/MPOL modes explicitly return `EOPNOTSUPP`. This is
process-local queue behavior, not a full futex2 implementation. The existing
classic futex queue still has its own synchronization and signal limits.

Validation: `./build.sh cli` and `python3 tests/run-futex-wait2.py` pass on
2026-11-18. The guest probe is a static AArch64 binary in a disposable root;
it covers errors, an expired timeout, and a real masked wake. No live session
or installed rootfs is modified.

Primary ABI sources:
[Linux futex syscalls](https://github.com/torvalds/linux/blob/master/kernel/futex/syscalls.c),
[Linux futex UAPI](https://github.com/torvalds/linux/blob/master/include/uapi/linux/futex.h).
