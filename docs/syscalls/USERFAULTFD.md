# `userfaultfd` (ARM64 syscall 282): fault protocol blocker

Shimmy currently returns `ENOSYS` for `userfaultfd`. Keep that behavior until
the descriptor's Linux fault protocol is implemented. A successful syscall
would have to return a pollable, readable descriptor that supports `UFFDIO_API`
and range registration. Guest accesses to registered pages would then have to
stop, emit correctly ordered `uffd_msg` records, and resume only after a
resolution operation such as `UFFDIO_COPY` or `UFFDIO_ZEROPAGE` (or a separate
`UFFDIO_WAKE` following `DONTWAKE`). Returning an ordinary pipe or eventfd
without these properties would mislead applications that depend on paging.

The runtime does have a Mach `EXC_BAD_ACCESS` receiver in `src/shim.c`, but its
current fault path calls `gmem_wx_fault()` to resolve registered guest JIT
write/execute transitions and diagnoses unhandled faults. It has no
userfaultfd registration table, missing-page/write-protect state, event queue,
or `UFFDIO_*` ioctl dispatch in `src/syscalls.c`. `gmem_mmap` and
`gmem_mprotect` also have to keep any new registrations coherent across
unmap, remap, fork, and protection changes. A new fault dispatcher must
coordinate with the existing JIT path without taking locks held by a stopped
guest thread; the JIT handler explicitly avoids its mapping lock for that
reason.

An eventual bounded first milestone could support only `UFFD_USER_MODE_ONLY`
and anonymous missing-page registration. Even that needs end-to-end tests of
faulting and handler threads, `poll/read`, `UFFDIO_API`, register/unregister,
copy/zero/wake, close while a fault is pending, and range mutations. Permission
to create a descriptor is not enough to count syscall 282 as working.

No runtime code changes in this branch; #282 remains `ENOSYS` and unimplemented.

Primary reference: [Linux kernel userfaultfd documentation](https://docs.kernel.org/admin-guide/mm/userfaultfd.html),
particularly the creation, initialization, registration, notification, and
resolution protocol.
