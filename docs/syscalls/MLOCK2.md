# ARM64 mlock2 (#284)

Linux `mlock2` accepts only flag 0 or `MLOCK_ONFAULT` (1). Flag 0 eagerly locks the rounded memory range. `MLOCK_ONFAULT` records deferred locking on the VMA; it cannot be represented by a Darwin `mlock` call ([Linux implementation](https://github.com/torvalds/linux/blob/master/mm/mlock.c)).

Shimmy now supports flag 0 for a fully mapped guest range of at most 1 MiB, rounded to the guest's 16 KiB pages. A zero-length request with either valid flag also succeeds without changing memory. It checks the guest range ledger and host mapping before calling Darwin `mlock`, so a guest cannot pin an unrelated host address. Bad flags and arithmetic overflow return `EINVAL`; unmapped ranges return `ENOMEM`; nonempty `MLOCK_ONFAULT` and larger ranges return `EOPNOTSUPP`. Host locking errors are translated to Linux errno. This is a bounded eager-lock subset, not Linux VMA accounting or full memlock resource-limit parity.

`python3 tests/run-mlock2.py` builds a static ARM64 guest in a disposable root and checks eager lock/unlock, unaligned rounding, invalid/unsupported flags, bounds, zero length, and unmapped memory.
