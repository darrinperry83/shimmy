# ARM64 fadvise64 (#223)

Shimmy currently supports a bounded `POSIX_FADV_NORMAL` call on a valid regular-file descriptor. This is a no-op because Shimmy does not yet change per-open-file readahead or cache policy, so every new descriptor is already in its normal state.

The four ARM64 arguments are `fd`, signed 64-bit `offset`, signed 64-bit `len`, and `advice`. The implementation returns Linux `EBADF` for invalid or shim-reserved descriptors, `ESPIPE` for a FIFO, and `EINVAL` for a negative offset or length or an invalid advice code. Recognized non-normal advice (`RANDOM`, `SEQUENTIAL`, `WILLNEED`, `DONTNEED`, `NOREUSE`) returns `EOPNOTSUPP` because Shimmy cannot yet carry out those page-cache or readahead requests. Other valid descriptor types are likewise outside the subset.

This replaces the previous unconditional success, which incorrectly told guests their request had been applied. The subset is verified by `python3 tests/run-fadvise64.py`, which builds a static ARM64 guest and runs it under `shim_cli` in a disposable root.

References: [Linux `mm/fadvise.c`](https://github.com/torvalds/linux/blob/master/mm/fadvise.c), [ARM64 generic syscall table](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h), and the ARM64 rootfs `linux/fadvise.h` values.
