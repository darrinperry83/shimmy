# ARM64 #47 `fallocate`: punch-hole mode validation

The existing handler entered Darwin `F_PUNCHHOLE` whenever the Linux
`FALLOC_FL_PUNCH_HOLE` bit was present. It did not require
`FALLOC_FL_KEEP_SIZE` or reject extra mode bits before the host operation.
Linux permits hole punching only with `PUNCH_HOLE | KEEP_SIZE`; the kernel
filesystem guidance states that the size must not change for that mode
(https://docs.kernel.org/filesystems/porting.html).

The narrow change rejects every other mode containing `PUNCH_HOLE` with
`EOPNOTSUPP` before a host file operation. The exact supported mode still
uses the existing Darwin path. The bounded `ZERO_RANGE` implementation and
mode-zero allocation path are unchanged.

A disposable raw ARM64 guest probe records an eight-byte file and verifies
that `PUNCH_HOLE` alone and `PUNCH_HOLE | KEEP_SIZE | unknown-bit` return
`EOPNOTSUPP` without changing file bytes or size. The same probe checks the
existing bounded `ZERO_RANGE | KEEP_SIZE` operation. Against the published
baseline the new probe fails on the first invalid punch mode; the candidate
passes. `./build.sh cli` and the existing `run-fallocate-zero-range.py`
regression pass. A private Arch boot reached a fresh LOGIN prompt with settled D-Bus; the
exact guest and disposable rootfs clone were cleaned. This is a startup
smoke test, not
proof of hole punching on every host filesystem.

Remaining limits: the exact valid punch mode depends on Darwin/filesystem
support and lacks a dedicated success probe. Linux sparse-extent semantics
are not established by this mode check. Concurrent file mutation remains
outside the single-call test.
