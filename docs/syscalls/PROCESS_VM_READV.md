# ARM64 process_vm_readv (#270)

Linux copies remote iovec bytes into local iovecs, with flags zero. Local buffers are validated before transfer; a remote page fault can return a partial byte count ([Linux manual](https://man7.org/linux/man-pages/man2/process_vm_readv.2.html)).

Shimmy's old handler raw-dereferenced every guest pointer, ignored target PID and flags, and could read shim host memory. The bounded replacement supports the current guest process only, at most 16 vectors and 1 MiB per side, with nonoverlapping source and destination regions. It validates local destination readability and writability before any remote transfer. Remote data is read through the guest memory ledger in 16 KiB page chunks, then written through checked guest copyout. A later invalid remote page returns the bytes already copied. Invalid vectors and other targets return explicit errors.

Cross-process reads and overlapping regions remain unsupported. The adjacent `process_vm_writev` bounded path is unchanged. `python3 tests/run-process-vm-readv.py` builds a static ARM64 guest in a disposable root and verifies direction, vector ordering, validation, and partial remote faults.
