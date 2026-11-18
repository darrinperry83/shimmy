# ARM64 #15 lremovexattr guest strings

The #15 dispatch snapshots both guest pathname and attribute name through
checked, bounded copy-in before calling the host. It resolves the path with
nofollow and removes the attribute from the link inode using
`XATTR_NOFOLLOW`. Invalid guest addresses return `-EFAULT` before mutation.

`python3 tests/run-lremovexattr-memory.py` uses a disposable root with a
symlink and target carrying distinct values for one attribute name. It checks
invalid addresses leave the link attribute intact, successful removal clears
only the link attribute, and the target retains its value. This does not
claim Linux-wide xattr namespace or filesystem equivalence; #14 and #16
remain separate handler work.
