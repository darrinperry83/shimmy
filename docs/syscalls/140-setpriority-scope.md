# ARM64 syscall 140: `setpriority` scope

Previously Shimmy forwarded guest `which` and `who` straight to Darwin.
`PRIO_USER`, `PRIO_PGRP`, or an unrelated positive PID could therefore
renice real macOS processes. Linux resolves these selectors in its own
[process namespace](https://github.com/torvalds/linux/blob/v6.12/kernel/sys.c#L217-L278).

The bounded path accepts `PRIO_PROCESS` for `who=0` or the current guest PID,
clamps nice to Linux's −20..19 range, and attempts to change only the current
host shim process. Darwin may still reject the self change with `EPERM` under
host sandbox policy; the shim does not fabricate a successful scheduling
change. An unknown PID returns `ESRCH`; a known guest child and group/user
selectors return `EOPNOTSUPP` until they have a safe guest-scoped model.
Invalid selectors return `EINVAL`. This does not provide child/group/user
priority management, Linux `CAP_SYS_NICE`, or guest-specific nice accounting.

`getpriority` is separately still a Darwin passthrough and needs its own
target-scope correction before claiming process-priority parity.

Verification: `./build.sh cli` and `python3 tests/run-setpriority-scope.py`.
The ARM64 probe uses impossible positive IDs for rejection tests, then
attempts to renice only its disposable guest process and checks the resulting
priority. In this sandbox the host returns `EPERM` even for a self renice, so
the probe accepts that documented host denial and verifies no priority change.
