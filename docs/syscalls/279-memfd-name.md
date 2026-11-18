# ARM64 #279 memfd_create guest name

The dispatch snapshots the guest name using checked, bounded copy-in before
the host `mkstemp` path. A bad address returns `-EFAULT` without creating a
host descriptor. A name without NUL in the 250-byte bound still returns
`-EINVAL`, matching the previous length check. Existing flag handling and
anonymous file backing are unchanged.

`python3 tests/run-memfd-name.py` runs an ARM64 guest in a disposable root.
It checks null/bad names, an overlong name, invalid flags, and a valid
executable static name that returns a writable, closeable descriptor.

This change does not establish full Linux memfd sealing, execution or
huge-page semantics; those require separate work.
