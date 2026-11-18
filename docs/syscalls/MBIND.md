# `mbind` (ARM64 syscall 235): default policy subset

The previous case returned success for every `mbind` request without recording a policy or migrating pages. This branch limits success to `MPOL_DEFAULT` with no mode flags, null nodemask, and no operation flags, over zero length or a fully mapped, readable guest range of at most 16 MiB. On Shimmy's single virtual NUMA node, that leaves the range under its existing default policy. It checks guest page alignment, length overflow, invalid mode/flag bits, host mapping continuity, and guest mapping provenance before success. An unmapped range returns `EFAULT`.

Valid nondefault policies, migration/strict requests, a supplied nodemask, large ranges, and guest ranges that are not readable return `EOPNOTSUPP`. Those paths need coherent per-VMA policy state, NUMA placement and migration, and compatible `get_mempolicy` behavior. This branch does not add such state. The guest page size is 16 KiB. Linux ignores `maxnode` when `nmask` is null; this subset does likewise.

Test: `./build.sh cli`, then `python3 tests/run-mbind.py`. The libc-free ARM64 probe uses a disposable root and checks mapped and unmapped ranges, zero length, alignment, overflow, invalid modes/flags and unsupported requests. It does not test actual page migration or a multi-node system.

Primary source: [Linux `mm/mempolicy.c`, `kernel_mbind` and `do_mbind`](https://codebrowser.dev/linux/linux/mm/mempolicy.c.html).
