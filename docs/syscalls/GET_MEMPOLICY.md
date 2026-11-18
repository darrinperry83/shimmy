# get_mempolicy (arm64 syscall 236)

Shimmy presents one virtual NUMA node. This implementation reports the current
process's `MPOL_DEFAULT` policy (0) with an empty policy nodemask, or node 0 in
the `MPOL_F_MEMS_ALLOWED` nodemask. It follows Linux's five-argument arm64 ABI
and word-rounded nodemask copy length, with a one-page maximum. Guest output
pointers are checked before writing.

`MPOL_F_ADDR` is explicitly unsupported because Shimmy does not maintain
per-VMA policy. `MPOL_F_NODE` on the default policy is invalid. Unknown flag
bits and invalid combinations return `EINVAL`. This is a truthful process
default query, not an implementation of NUMA placement or migration.

Run `./build.sh cli` and `python3 tests/run-get-mempolicy.py`. The disposable
ARM64 guest checks default policy, allowed node, mask length and zero-padding,
invalid requests, and invalid output pointers.
