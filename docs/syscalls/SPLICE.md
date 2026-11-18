# ARM64 syscall 76: splice

The shim supports a bounded copy between a pipe and one other descriptor. It
accepts Linux's four splice flags, validates descriptor access, rejects append
output, and implements the file-side offset pointer and implicit file position.
At most 1 MiB is transferred per call. Guest offset pointers are checked before
the transfer and written back with the number of bytes reported transferred.
Zero length returns zero without validating descriptors or flags, as Linux does.

Pipe-to-pipe transfers return `EOPNOTSUPP`. Darwin reports different inode
numbers for the read and write ends of one pipe, so the existing `fstat` check
cannot distinguish a self-transfer from a transfer between two pipes. Allowing
a self-transfer can block indefinitely. This requires a pipe-identity registry
that survives descriptor duplication and process inheritance before support
can be added safely.

The implementation uses a bounce buffer rather than zero-copy. Its
`SPLICE_F_NONBLOCK` path temporarily changes the source descriptor's
`O_NONBLOCK` flag, which is shared by duplicated descriptors. A partial output
write after reading from a pipe can also consume more input than the reported
transfer. Those cases need a descriptor-local nonblocking operation and a
proper pending-buffer model for stronger Linux compatibility.

`tests/run-splice.py` compiles a disposable ARM64 guest and checks both
file-to-pipe and pipe-to-file paths, explicit and implicit offsets, and error
cases. The harness does not modify the installed rootfs.
