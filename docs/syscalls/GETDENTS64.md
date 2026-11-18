# `getdents64` (#61)

The existing translator packs Linux `dirent64` records from either a host
directory or a synthetic `/proc`, `/sys` or `/dev` entry table. It previously
wrote records directly through the guest pointer. An inaccessible guest
destination could therefore be detected only after the host or synthetic
directory cursor had advanced.

The ARM64 dispatch now rejects shim-owned descriptors, checks the guest output
range before enumeration, stages records in a host buffer and copies out only
the bytes returned by the translator. It caps one call's effective capacity at
1 MiB; returning fewer records than requested is legal for `getdents64`.
The translator retains its record packing and small-buffer `EINVAL` behavior.

`tests/run-getdents-memory.py` builds a disposable ARM64 guest. It checks a
real directory and synthetic `/proc`: an invalid or executable read-only
output returns `EFAULT` without advancing the cursor, and a one-byte buffer
returns `EINVAL` before the first record. The next valid stack-buffer result
is compared with an independently opened control descriptor, checking both
the records and cursor position. Static output and an invalid descriptor are
also covered.

Guest mappings can change between preflight and copyout, so a late `EFAULT`
after enumeration may still leave a cursor advanced. This does not yet make
directory reads atomic with respect to concurrent guest memory changes. The
isolated private Arch boot reached a fresh login prompt with D-Bus broker and
launcher settled; its exact guest and clone were cleaned.
