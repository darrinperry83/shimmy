# `userfaultfd` Mach fault relay proof, not guest support

`tests/run-uffd-mach-fault.sh` runs a standalone native test process. It
creates one new anonymous `PROT_NONE` page and keeps the page private until
registration. That creation order is the eligibility proof for this test:
no worker could have touched the page beforehand. A worker installs a
**thread-scoped** Mach `EXC_BAD_ACCESS` port and accesses the page. The
exception handler reports the real fault to the private UFFD backend and
waits. The pager sees poll readiness, reads the event, encodes a Linux
`uffd_msg`, changes the page to read/write, writes a byte, and completes the
fault. The worker resumes at the same instruction and reads that byte.
The test cleans up the Mach port and threads. Twenty consecutive runs passed.

This proves the Darwin exception-to-queue-to-wakeup mechanism in an isolated
process. It does **not** enable syscall 282, implement `UFFDIO_COPY`, or prove
the behavior in a Shimmy guest. Existing guest JIT protection faults are still
handled by `gmem_wx_fault()` in `src/shim.c`; the test does not intercept them
or change that path. No runtime source is changed by this branch.

The remaining memory eligibility problem is concrete. `gmem_mmap` maps
ordinary anonymous read/write pages directly, so a guest may have written
them before a later `UFFDIO_REGISTER`. It does not record per-page first
touch. For anonymous `PROT_NONE`, `gmem_mmap_locked` calls `gmem_reserve`,
which creates a MAP_JIT reservation with host read/write/execute protection
and special per-thread write controls. Guest `PROT_NONE` intent therefore
does not prove an untouched host page. Reclassifying those reservations as
missing pages would collide with the existing JIT and dynamic-loader paths.

A production implementation needs guest mapping provenance plus reliable
per-page missing/present state from mapping creation, safe classification of
Mach faults ahead of the JIT handler, and Linux fd/ioctl lifecycle. The next
proof must use a real ARM64 guest and `UFFDIO_API`/`REGISTER`/`COPY` or
`ZEROPAGE`/read/poll/close. Until then `userfaultfd` stays `ENOSYS`.
