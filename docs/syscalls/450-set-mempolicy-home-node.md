# ARM64 syscall 450: `set_mempolicy_home_node`

Linux changes the home node only for existing per-VMA `MPOL_BIND` or
`MPOL_PREFERRED_MANY` policies. Its [kernel implementation](https://github.com/torvalds/linux/blob/v6.12/mm/mempolicy.c#L1450-L1512)
returns `ENOENT` when it finds no policy in a nonempty range.

Shimmy exposes only node 0 and does not install per-VMA memory policies.
The bounded implementation validates the 16 KiB guest-page alignment,
zero flags, node 0 and range arithmetic. It accepts a zero-length range and
returns `ENOENT` for a valid nonempty range. This is accurate for the present
guest policy model; it does **not** implement home-node changes or NUMA placement.
If per-VMA bind/preferred-many support is added, this handler must be revisited.

Verification: `./build.sh cli` and `python3 tests/run-set-mempolicy-home-node.py`.
The probe covers mapped/unmapped ranges, zero length and invalid arguments.
