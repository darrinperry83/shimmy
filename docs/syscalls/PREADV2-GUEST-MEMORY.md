# `preadv2` (#286): checked guest vectors

The prior ARM64 handler interpreted the x3 offset and x5 flags correctly,
but passed the guest iovec array and destination addresses directly to
Darwin `preadv` or `readv`. The handler now uses the established bounded
`preadv` (#69) path for nonnegative offsets. At offset `-1`, it snapshots up
to 1024 guest iovecs, validates the writable destination prefix, reads up to
1 MiB into a private buffer at the file description's current offset, then
copies only the bytes read into guest memory. The existing x4 ignore and
nonzero x5 refusal remain unchanged. This preserves current-offset reads on
ordinary files and pipes without handing guest addresses to host I/O.

`./build.sh cli` passed. `tests/run-preadv2.py` passed the ARM64 positioned
and current-offset cases, the >4 GiB sparse offset, flags, bad iovec and
destination pointers, and a pipe read. A separate private Arch clone reached
a fresh login with D-Bus broker and launcher settled. The harness stopped
its exact guest, and the clone was removed. Larger requests may return a
short read at the 1 MiB bound; nonzero RWF flags and broader descriptor
semantics remain unsupported. This does not establish full Linux `preadv2`
compatibility or a GUI boot.
