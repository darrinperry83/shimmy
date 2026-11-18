# `getrusage` (AArch64 syscall 165)

Shimmy translates Darwin process and reaped-child usage into the 144-byte
AArch64 Linux `struct rusage`. The output is copied through checked guest
memory. `RUSAGE_SELF` (0) and `RUSAGE_CHILDREN` (-1) are supported.

`RUSAGE_THREAD` (1) returns `EOPNOTSUPP`: Darwin's `getrusage` has no matching
per-thread selector, and reporting the whole process would be misleading.
Other selectors return `EINVAL`; null or invalid output memory returns
`EFAULT`. The host process usage is a bounded approximation for the guest
process, including shim overhead, and no complete Linux accounting parity is
claimed.

`python3 tests/run-getrusage.py` builds and executes a disposable AArch64
guest probe covering selectors, Linux output shape, and invalid pointers.
