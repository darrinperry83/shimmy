# ARM64 #35 unlinkat guest pathname

The handler now snapshots a bounded guest pathname before resolving it or
removing a host file or directory. Invalid and unterminated addresses return
`-EFAULT` or `-ENAMETOOLONG` without reaching host mutation. Existing
`AT_REMOVEDIR` and cgroup handling are unchanged.

`python3 tests/run-unlinkat-path.py` runs a disposable ARM64 guest. It
checks malformed/NULL/unterminated addresses, regular file removal,
directory removal, and preservation of an unrelated file.

This is distinct from the previously withheld #35 flag-validation branch.
Unknown flag handling remains uncorrected and must not be counted as fixed
by this change. Broader path races and filesystem semantics also remain.
