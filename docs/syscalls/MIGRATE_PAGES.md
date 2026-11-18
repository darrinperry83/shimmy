# `migrate_pages` (ARM64 syscall 238): one-node subset

Shimmy exposes one virtual NUMA node, node 0. Linux's `migrate_pages` copies old/new nodemasks, finds the target task, then maps source nodes to destination nodes. Its return value is the number of pages that could not be moved. If both masks contain only node 0, or either is empty, there is no different destination node and the result is 0. This branch supports that no-migration case for the calling guest process. It does not move memory.

`maxnode` is one greater than the highest node bit. A value of 1 ignores mask pointers, as Linux's `get_nodes()` does. With `maxnode` 2–65, the single word of each non-null mask is copied through checked guest memory access; bits beyond `maxnode - 1` are ignored. Invalid pointers return `EFAULT`, and a set bit for an unmodeled node returns `EOPNOTSUPP`. Wider masks are outside the bounded subset. A known guest child target returns `EOPNOTSUPP`; an absent PID returns `ESRCH`. This does not promise cross-process migration or physical NUMA placement.

Run `./build.sh cli` and `python3 tests/run-migrate-pages.py`. The disposable ARM64 probe covers self no-op masks, absent PID, ignored mask pointers, bad pointer, invalid width and unsupported node. It does not test a multi-node guest.

Primary source: [Linux `mm/mempolicy.c`, `kernel_migrate_pages`, `get_nodes`, and `do_migrate_pages`](https://github.com/torvalds/linux/blob/master/mm/mempolicy.c).
