# ARM64 getrlimit (163)

`getrlimit` now validates the Linux resource number and copies the 16-byte
result through the checked guest-memory path. Resources 0–4 and 6–9 use the
corresponding Darwin limit. Linux RLIMIT_RSS (5) is unsupported because Darwin
aliases it to RLIMIT_AS; Linux resources 10–15 also return `EOPNOTSUPP` until
guest state and enforcement exist. Out-of-range resources return `EINVAL`.
These errors leave the caller's output untouched.

RLIMIT_NOFILE's guest-visible soft limit is capped at 4096, matching Shimmy's
existing `prlimit64` behavior. That cap prevents very slow close-before-exec
loops through the syscall trap. It is a presentation cap; the host limit still
governs opened descriptors. `setrlimit` and `prlimit64` can still modify the
host-backed limits, so the broader per-guest resource-limit contract remains
incomplete. The mapping of Darwin NPROC/MEMLOCK and other resources is a
bounded approximation, not full Linux enforcement parity.

`python3 tests/run-getrlimit.py` exercises the ARM64 ABI, invalid pointers,
unsupported resources, and NOFILE consistency with `prlimit64` in a disposable
guest root. It does not test the installed rootfs or GUI.
