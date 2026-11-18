# 266 `clock_adjtime`

Shimmy supports only the read-only `modes=0` query for `CLOCK_REALTIME`,
using its tested `adjtimex` translation. A null or unmapped Linux `timex`
returns `EFAULT`; nonzero adjustment modes return `EOPNOTSUPP` without
changing the host clock. Other clock IDs return `EINVAL`. This is a bounded
query, not permission to discipline macOS time or proof of full Linux NTP
clock behavior.

`python3 tests/run-clock-adjtime.py` tests the ARM64 guest ABI, errors,
non-mutation on refused adjustment, and the read-only real-time result.
