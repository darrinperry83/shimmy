# `adjtimex` (171): read-only host clock query

The old implementation returned `TIME_OK` with 208 zero bytes for every input,
including a null pointer and requests to adjust the clock. That falsely
reported a synchronized clock and successful changes.

This branch supports only `modes=0`. It queries macOS with `ntp_adjtime` and
`ntp_gettime`, translates the native AArch64 Linux 208-byte `struct timex`,
and returns the host clock state (including `TIME_ERROR` when reported). The
fields available in both APIs are copied. Current time uses the host NTP
timestamp; the subsecond unit follows `STA_NANO`. `tick` follows Shimmy's
existing virtual 100 Hz clock. PPS counters can be zero when the host does not
maintain PPS data. The guest buffer must be readable and writable.

Any nonzero `modes` returns `EOPNOTSUPP` without changing the host clock or the
guest structure. Missing or invalid buffers return `EFAULT`. This is a bounded
status query, not Linux clock discipline, namespace, privilege or precision
parity. The query describes the shared Mac clock because Shimmy has no virtual
NTP clock. It may be unavailable on macOS installations where the native NTP
query fails, in which case the host error is translated.

Proof: `./build.sh cli` and `python3 tests/run-adjtimex.py` passed on
2026-11-18. The ARM64 probe checks null/bad pointers, a rejected write request
without guest-buffer mutation, a plausible nonzero timestamp, units, and
virtual tick. It does not test active clock adjustments or all NTP states.
