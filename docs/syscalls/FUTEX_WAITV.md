# `futex_waitv` (ARM64 syscall 449)

Linux accepts up to 128 `struct futex_waitv` entries and waits atomically for a wake on any address. Each entry is `{u64 val, u64 uaddr, u32 flags, u32 reserved}`; success returns the index that woke. See Linux's [syscall implementation](https://codebrowser.dev/linux/linux/kernel/futex/syscalls.c.html#L317) and [UAPI structure](https://codebrowser.dev/linux/linux/include/uapi/linux/futex.h.html#L95).

Shimmy supports exactly one private, aligned U32 waiter. It uses the existing futex2 wait path with an all-bits wake mask and returns index zero on a wake. It validates the vector pointer, count, top-level flags, entry layout, and the same value, timeout, and address rules as `futex_wait`. Invalid counts return `EINVAL`; vectors of 2–128 entries return `EOPNOTSUPP` without parking. Shared and NUMA entries remain unsupported.

Multi-address waits need atomic registration across buckets and wake-index ownership. Reusing independent one-address waits would allow lost wakes and must not be advertised as `futex_waitv` support. The disposable static ARM64 guest test covers errors, mismatch, timeout, and a real one-address wake. It does not establish cross-process, signal interruption, or full vector compatibility.
