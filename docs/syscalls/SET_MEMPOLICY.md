# `set_mempolicy` (ARM64 syscall 237): default-only subset

The previous handler returned success without changing policy for every request. This branch permits only `MPOL_DEFAULT` with no effective nodemask. That operation leaves Shimmy's one-node guest on its existing default allocation policy. A NULL mask is accepted; with a one-bit `maxnode`, Linux ignores even a non-NULL mask. For `maxnode` 2–65, the guest mask is copied safely and the relevant bits must be zero. Invalid modes and mutually exclusive flags return `EINVAL`; valid nondefault modes and unsupported mode flags return `EOPNOTSUPP` rather than claiming NUMA placement. Wider non-NULL masks remain unsupported.

This is not an implementation of preferred, bound, interleaved, local, or weighted placement. It does not alter allocation, VMA policy, or per-thread policy state. `get_mempolicy` and `mbind` are reviewed in separate branches and must not claim nondefault behavior. The supported reset is truthful only while those calls likewise reject unsupported policy state.

Focused disposable ARM64 guest test: `python3 tests/run-set-mempolicy.py --sysroot <installed Arch root>`. It covers the default reset, effective and ignored mask bits, bad guest pointers, invalid flags, and unsupported modes. CLI build is required before integration.

Primary sources: [Linux `mm/mempolicy.c`, `kernel_set_mempolicy` and `get_nodes`](https://codebrowser.dev/linux/linux/mm/mempolicy.c.html#1835), [Linux UAPI `mempolicy.h`](https://codebrowser.dev/linux/linux/include/uapi/linux/mempolicy.h.html#18).
