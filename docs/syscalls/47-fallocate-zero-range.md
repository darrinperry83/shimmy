# ARM64 #47 `fallocate`: bounded `ZERO_RANGE`

Shimmy previously returned `EOPNOTSUPP` for `FALLOC_FL_ZERO_RANGE`. It now
accepts `ZERO_RANGE` with optional `KEEP_SIZE` on writable regular files when
the whole range is within the current file size and at most 1 MiB. Positioned
zero writes make reads of the range return zero while preserving the current
file offset and size. `O_APPEND` is refused because it could redirect a
positioned write on some hosts. Invalid flag combinations, unsupported
past-EOF ranges and larger ranges fail before writing.

This is a data-visible subset, not Linux unwritten-extent or sparse-allocation
parity. It may allocate blocks and perform real I/O. An I/O error partway
through can leave earlier bytes zeroed, as a mutating operation may. Concurrent
truncation is not coordinated and can invalidate the prechecked EOF bound. The
existing mode-0, `KEEP_SIZE` and punch-hole paths remain otherwise unchanged;
the common signed `off + len` overflow is now rejected before host work.

Proof: `./build.sh cli`; `python3 tests/run-fallocate-zero-range.py --sysroot
<Arch rootfs>` exercises the ARM64 guest syscall against a disposable file,
checking zeroed and adjacent bytes, unchanged seek offset and size,
`KEEP_SIZE`, invalid combinations, past EOF, read-only and append descriptors.
A private Arch login/D-Bus boot smoke checks startup compatibility, not
allocation behavior on every filesystem.
