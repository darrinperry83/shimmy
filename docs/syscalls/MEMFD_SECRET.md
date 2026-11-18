# `memfd_secret` (ARM64 syscall 447): blocked by secret-memory isolation

Linux's `memfd_secret(flags)` creates a descriptor for pages whose direct kernel mapping is removed. The pages are unswappable, excluded from core dumps, accounted against `RLIMIT_MEMLOCK`, zeroed when freed, and usable through a shared mapping of the descriptor. The current Linux implementation accepts only zero or `O_CLOEXEC` flags when the facility is available; it returns `ENOSYS` when secret memory is disabled or the architecture cannot invalidate the direct map.

Shimmy has no secret-memory page allocator or direct-map isolation backend. Its `memfd_create` uses an unlinked Darwin temporary file, while `gmem_mmap` maps ordinary host pages. Wrapping that file with `mlock` or `MADV_DONTDUMP` would not prevent the host kernel or other host facilities from accessing the contents and would not provide Linux's page lifecycle, accounting, and mapping restrictions. Returning a successful fd here would falsely promise a security property.

Keep syscall 447 on the existing `ENOSYS` fallback until a dedicated backend can demonstrate the isolation contract. Before enabling it, test flags and descriptor lifecycle, shared-only mapping, page locking and limit accounting, dump exclusion, zero on release, and access denial from outside the authorized mapping. An ordinary `memfd_create` test cannot validate this syscall.

Primary source: [Linux `mm/secretmem.c`, including the page fault, mapping, and syscall paths](https://github.com/torvalds/linux/blob/master/mm/secretmem.c).
