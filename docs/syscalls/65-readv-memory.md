# ARM64 syscall 65: checked guest-output bridge

The old `readv` route passed guest iovec addresses directly to Darwin. A
host read could consume pipe/socket data before discovering that a guest
destination was an inaccessible ELF/JIT range. The new bounded route copies
the iovec list through the guest range ledger, checks every nonempty
destination for writability without writing to it, and refuses requests over
1 MiB before reading. A larger read could truncate and discard the remainder
of a datagram, so it returns `EOPNOTSUPP` instead of silently limiting the
host call.

For admitted ranges, Shimmy reads once into host-owned buffers and copies
only the returned byte prefix into the guest. It returns the exact host count
on success. On a late copyout failure after some bytes have been delivered,
it returns that delivered prefix; if none were delivered, `EFAULT`. A guest
mapping can still change between the writable-range preflight and copyout,
so this is not a guarantee of Linux-equivalent behavior under concurrent
`munmap`/`mprotect` races. This path does not write through guest JIT memory
using a host `readv` pointer.

Proof: `./build.sh cli` and `python3 tests/run-readv-memory.py`. The ARM64
probe covers stack and static writable outputs, partial EOF read across two
vectors, malformed vector and payload addresses without consuming queued
pipe bytes, read-only executable/JIT output rejection, and count/byte bounds.
An isolated private Arch boot reached a fresh login prompt with the D-Bus
broker and launcher settled. Its exact guest and clone were cleaned. The
boot smoke does not verify every long-running application or concurrent
memory-map race.
