# ARM64 #6 lsetxattr checked guest input

The #6 handler snapshots bounded pathname, name and entire value through
checked guest copy-in before changing the host link inode. Linux's 64 KiB
value bound and CREATE/REPLACE flags are checked; the host call retains
`XATTR_NOFOLLOW`. An unreadable later page returns `-EFAULT` without
changing the existing attribute.

`python3 tests/run-lsetxattr-memory.py` checks a disposable symlink and
target with distinct attribute values. It covers CREATE/REPLACE, invalid
flags and addresses, oversized input, a value crossing into a protected
page, and confirms that writing the link leaves its target unchanged.

This is a bounded guest-memory and symlink-semantics correction. It does not
claim Linux-wide xattr namespace or filesystem parity. #5 and #7 remain
separate handlers.
