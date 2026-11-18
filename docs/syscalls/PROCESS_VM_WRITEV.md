# ARM64 process_vm_writev (#271)

Linux copies local iovec bytes into another process's remote iovecs. Flags must be zero, vector counts are limited to `IOV_MAX`, local buffers are validated before transfer, and a remote fault may produce a partial count ([Linux manual](https://man7.org/linux/man-pages/man2/process_vm_writev.2.html)).

Shimmy previously used raw `memcpy` on unvalidated guest pointers without checking the target PID or flags. Because the shim and guest share a host address space, that could write unrelated host memory.

The bounded replacement supports only the current guest process, at most 16 vectors per side and 1 MiB per side, with no overlap between local and remote ranges. It snapshots and validates every local vector before writing. Remote writes use the checked guest memory ledger, one guest page at a time, and return the copied byte count if a later remote page is invalid. Invalid or absent PID, flags, vector arrays, and guest buffers receive explicit errors. Another known guest process is unsupported until there is a safe cross-process memory broker. `process_vm_readv` is unchanged by this commit and requires its own audit.

`python3 tests/run-process-vm-writev.py` builds a static ARM64 guest in a disposable root and checks content, validation, overlap rejection, and partial remote failure.
