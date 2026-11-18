# `vmsplice` (#75) checked pipe-write subset

The existing Shimmy implementation already copied iovec bytes into a pipe and
handled zero vectors, flag validation, and its bounded nonblocking policy.
It passed guest pointers directly to host `writev`, though, and did not bound
the total guest data passed in one call.

This change snapshots up to 1,024 Linux iovecs through checked guest memory,
gathers at most 1 MiB of their bytes into a host buffer, then issues one pipe
write. An invalid iovec array or data range returns `EFAULT` before any pipe
write. A positive short host write returns its actual byte count. The existing
behavior for invalid flags, non-pipe descriptors, and `SPLICE_F_NONBLOCK` on a
blocking pipe remains. `SPLICE_F_MOVE`, `MORE`, and `GIFT` remain hints; the
implementation copies bytes rather than lending pages.

Linux [`fs/splice.c`](https://codebrowser.dev/linux/linux/fs/splice.c.html)
imports an iovec and supports memory-to-pipe and pipe-to-memory directions.
Shimmy implements only memory-to-pipe here. After `./build.sh cli`, run
`python3 tests/run-vmsplice-checked.py --sysroot /path/to/arch/rootfs` for the
disposable raw ARM64 probe. It tests data order, zero vectors, invalid flags
and pointers, no write for a bad later vector, nonblocking policy, and a
non-pipe descriptor.

This does not provide page gifting, the reverse pipe-to-memory direction,
Linux pipe buffer identity, or a full boot/desktop regression result.
