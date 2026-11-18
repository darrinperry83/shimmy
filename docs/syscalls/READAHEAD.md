# `readahead` (ARM64 syscall 213)

Shimmy now validates a readable regular-file descriptor and nonnegative
offset, then requests asynchronous file-cache loading with Darwin's
`F_RDADVISE`. It returns zero on a successful advisory, without changing the
file offset. A zero count means offset through EOF; an offset at or beyond EOF
needs no hint and returns zero after descriptor validation. Invalid or
write-only descriptors return `EBADF`; unsupported file types and negative
offsets return `EINVAL`.

Darwin's `radvisory.ra_count` is an `int`, so a request spanning more than
`INT_MAX` bytes through EOF returns `EOPNOTSUPP` rather than being silently
truncated. Filesystem-specific failures from `F_RDADVISE` are propagated.
This supports regular files only; Linux can also readahead block devices.

Linux checks read mode and file type before forwarding the hint through
`vfs_fadvise(..., POSIX_FADV_WILLNEED)`: [Linux `mm/readahead.c`](https://github.com/torvalds/linux/blob/master/mm/readahead.c).

Verification: `./build.sh cli` and `python3 tests/run-readahead.py`. The test
compiles a static ARM64 probe into a disposable rootfs.
