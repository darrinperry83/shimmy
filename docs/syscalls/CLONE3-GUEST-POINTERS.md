# `clone3` (#435): checked mandatory input and flagged outputs

The effective `clone3` handler is `do_clone3` in `src/shim.c`. It previously
read the 64-byte mandatory `clone_args` directly through a guest address. An
invalid address could fault Shimmy's host exception thread. Its debug-only
pidfd log also dereferenced a guest address directly. This increment takes a
checked snapshot of the mandatory fields and uses checked copyin for that
debug value. An invalid input address returns `EFAULT` without creating a
child.

The shared clone path writes flagged output pointers directly. `do_clone3`
now checks that its pidfd, parent TID, and child TID output ranges are writable
before invoking that path. Invalid outputs return `EFAULT` before creating a
child. The shared `clone` implementation and ordinary valid `clone3` paths
are otherwise unchanged.

The disposable, no-libc ARM64 probe `tests/run-clone3-debug-copyin.py` runs
with `RDCLONEDBG=1`. It checks invalid input, short argument size, invalid
pidfd/parent-TID/child-TID outputs, no child after a bad pidfd pointer, an
ordinary fork-style child and wait, and a valid pidfd child and wait. The CLI
build passes. A private Arch clone reached a fresh login prompt with D-Bus
broker and launcher settled; the harness stopped its exact guest and the
disposable clone was removed.

This is bounded pointer validation, not full `clone3` conformance. An output
mapping can change after preflight, so a late unmap race remains. The handler
only snapshots the first 64 bytes and does not validate the extended
`clone_args` version fields, all flag combinations, stack/TLS contracts,
namespaces, or broad thread and pidfd lifetime semantics. Those remain
separate work.
