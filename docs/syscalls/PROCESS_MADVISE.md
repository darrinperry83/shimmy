# `process_madvise` (ARM64 syscall 440)

Shimmy supports one iovec aimed at its own process through a Shimmy kqueue
pidfd, with `flags == 0` and `MADV_NORMAL`. It verifies the pidfd, copies the
guest iovec through the guest memory ledger, checks page alignment and full
mapped coverage, and returns the **original byte length** on success. A zero
length iovec returns zero. A zero-vector request also returns zero after
checking the pidfd and behavior.

`MADV_NORMAL` resets a Linux VMA to normal advice. Shimmy has not implemented
other advice state, so this self-process case needs no native memory change.
Other advice and multiple iovecs return `EOPNOTSUPP`. Remote targets return
`EOPNOTSUPP` and cannot operate on a host process address space. Invalid flags,
bad descriptors/pointers, misaligned ranges, and unmapped ranges return errors
instead of the previous unconditional success. The guest memory ledger may
reject an otherwise valid mapping not yet represented in that ledger; this is
a bounded implementation, not complete Linux `process_madvise` compatibility.

The Linux implementation validates flags, imports the iovec, resolves the
pidfd, checks the advice, and returns the number of bytes advised by
`vector_madvise`: [Linux `mm/madvise.c`](https://github.com/torvalds/linux/blob/master/mm/madvise.c).

Verification: `./build.sh cli` and `python3 tests/run-process-madvise.py`.
The latter compiles a disposable static ARM64 guest, checks the success byte
count and explicit rejection paths, then deletes its temporary rootfs.
