# ARM64 syscall 66: checked guest-memory bridge

Darwin `writev` cannot reliably read Linux ELF/JIT-backed guest payloads
directly. The previous route passed guest iovecs and payload pointers to
Darwin and returned `EFAULT` for a static executable-segment string even
though the guest could read it. The host trace path also read raw guest
buffers after a successful write.

Shimmy now copies up to Linux's 1024 iovecs through the admitted guest range
ledger, checks the total length for overflow, then snapshots up to 1 MiB
of payload into host memory before one host `writev`. Invalid descriptors or
payloads return `EFAULT` without a host write. Larger requests return
`EOPNOTSUPP` before writing: truncation could corrupt a datagram or record
oriented socket message. Host short writes within the bound keep their actual
byte count, including under trace. The trace formats the host snapshot.
This is a bounded compatibility path, not a promise that every guest range
or concurrent memory mutation is Linux equivalent.

The code returns the host `writev` count unchanged for short writes. A
reliable forced-short-write fixture is still needed: the attempted
nonblocking pipe case produced `EAGAIN` after filling and partially draining
the pipe, so it was not retained as a regression assertion.

`./build.sh cli`, `python3 tests/run-writev-memory.py`, and the prior
`python3 tests/run-writev-return.py` verify the CLI and disposable ARM64
guests. The new probe covers stack, static writable data, executable/JIT
data, invalid vector/payload pointers, no partial host write on bad payload,
vector count and byte bounds, and trace off/on. The isolated private Arch
boot reached a fresh login prompt with D-Bus broker and launcher settled;
the exact guest and clone were cleaned. This does not verify every application
or long-running desktop workload.
