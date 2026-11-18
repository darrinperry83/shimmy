# `sync_file_range` (#84)

The old handler ignored offset, length and flags, then ran Darwin `fsync` over
the entire file. That does not implement the caller's selected range or its
Linux wait/write semantics.

The bounded implementation validates the descriptor, signed offset/length,
overflow, supported flag bits, and file type. A valid request with flags zero
returns success because Linux performs no writeback for that combination.
That works for ordinary regular and directory descriptors. Requests with
`SYNC_FILE_RANGE_WAIT_BEFORE`, `SYNC_FILE_RANGE_WRITE`, or
`SYNC_FILE_RANGE_WAIT_AFTER` return `EOPNOTSUPP`; unknown flag bits or invalid
ranges return `EINVAL`; closed descriptors return `EBADF`; pipes return
`ESPIPE`. No host `fsync` runs for this call.

Linux [`fs/sync.c`](https://codebrowser.dev/linux/linux/fs/sync.c.html)
checks the range and flags, allows regular/block/directory/symlink files, and
only invokes write/wait operations when those bits are set. This change does
not provide range writeback, synchronization, or writeback error reporting.

After `./build.sh cli`, run `python3 tests/run-sync-file-range.py --sysroot
/path/to/arch/rootfs` for the disposable raw ARM64 guest probe. Integration
should include a private boot regression because callers that relied on the
old whole-file flush will now receive an error for nonzero flags.
