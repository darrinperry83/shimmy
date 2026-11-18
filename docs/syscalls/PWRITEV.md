# `pwritev` (#70)

The previous handler passed the guest iovec array and guest data pointers
directly to Darwin `pwritev` and narrowed the vector count to `int` without
checking it. The bounded implementation snapshots up to 1,024 vectors through
checked guest memory, gathers up to 1 MiB of their data into a host buffer,
then issues one positioned `pwrite`. If any gathered guest buffer is invalid,
it returns `EFAULT` before writing. A positive short host write returns its
actual byte count. Explicit-position writes do not move the file description's
current offset.

The source must be a writable ordinary regular file. A negative signed offset,
oversized vector count, aggregate length overflow or position overflow is
`EINVAL`; invalid or read-only descriptors are `EBADF`; pipes return `ESPIPE`;
append mode is `EOPNOTSUPP` because its positioned-write behavior differs
between hosts. On 64-bit ARM64, the entire signed offset is in x3; x4 is
irrelevant, as in Linux
[`fs/read_write.c`](https://codebrowser.dev/linux/linux/fs/read_write.c.html).

After `./build.sh cli`, run `python3 tests/run-pwritev.py --sysroot
/path/to/arch/rootfs`. The disposable probe checks vector ordering, content,
unchanged current position, a 5 GiB sparse write, bad vector array/data,
no-write behavior for a bad later vector, zero vectors and descriptor errors.

This is not Linux's full vectored I/O implementation. A write larger than
1 MiB can return short, and concurrent mutation of guest buffers, device
writes, append semantics, and writeback durability need separate work. A full
boot regression is required before integration.
