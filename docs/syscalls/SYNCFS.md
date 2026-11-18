# `syncfs` (#267)

The previous handler called Darwin `sync()` and returned success for every
descriptor, including closed ones. That could tell a Linux application its
filesystem had been synchronized when macOS had only scheduled writeback.

The new bounded behavior checks the guest-backed descriptor with `F_GETFD`.
Invalid or closed descriptors return `EBADF`. Valid descriptors return `ENOSYS`
without invoking `sync()`. This is an explicit unsupported operation, not a
filesystem durability claim.

Linux [`fs/sync.c`](https://codebrowser.dev/linux/linux/fs/sync.c.html) shows
that `syncfs` validates its descriptor, waits for its superblock's writeback,
and reports filesystem writeback errors. Apple's
[`sync(2)` manual](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/sync.2.html)
says it may return before buffers finish flushing. Darwin's `fsync` and
`F_FULLFSYNC` act on individual files; they do not supply the Linux operation
over all dirty files on the descriptor's filesystem.

After `./build.sh cli`, run `python3 tests/run-syncfs.py --sysroot
/path/to/arch/rootfs`. The disposable ARM64 probe checks regular and directory
descriptors plus closed and negative descriptors. Before integration, rerun
the private systemd boot gate because applications that relied on the false
success may now handle `ENOSYS` differently. This change does not provide
actual `syncfs` durability.
