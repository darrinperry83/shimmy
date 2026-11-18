# `gettimeofday` (AArch64 syscall 169)

The shim reads the current host UTC clock, translates the Darwin `timeval`
into Linux's two 64-bit fields, and writes optional guest outputs through
checked guest memory. The obsolete timezone output remains two zero 32-bit
fields, consistent with the shim's existing UTC presentation. Invalid guest
output pointers return `EFAULT`; either output may be omitted.

`python3 tests/run-gettimeofday.py` builds a disposable AArch64 guest and
checks the layout, microsecond range, optional outputs, and invalid pointers.
This does not provide a separate guest clock namespace or guarantee clock
synchronization quality beyond the host clock.
