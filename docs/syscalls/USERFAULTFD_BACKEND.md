# `userfaultfd` internal event backend prototype

This branch adds `src/uffd_backend.c/h` as an isolated, guest-private state
machine. It is **not linked into the guest runtime** and does not enable
syscall 282. The current `ENOSYS` response remains the only guest-visible
behavior.

The backend models bounded 16 KiB aligned ranges, rejects overlaps, assigns
ordered fault sequence numbers, exposes a private pollable notification fd,
provides nonblocking event dequeue, waits for individual fault completion,
and cancels blocked reporters when a range is unregistered or the backend is
destroyed. Its readiness pipe is internal: consumers must not read it
directly. This supports a future Linux `read/poll` adapter but is not itself a
Linux userfaultfd descriptor.

`sh tests/run-uffd-backend.sh` compiles the standalone backend with warnings
as errors and passes an end-to-end host-thread lifecycle test. The test covers
range validation/overlap, nonblocking empty reads, poll notification,
event/completion pairing, eight concurrent reporters resolved in reverse
order, unregister cancellation, and destruction with a blocked reporter.
Thirty consecutive test runs passed on 2026-11-18.

The missing pieces before syscall exposure are substantial: validate that
registered addresses are appropriate guest anonymous mappings; instrument
the Mach fault path without breaking JIT W^X; distinguish missing faults from
protection faults; implement `UFFDIO_API`, `UFFDIO_REGISTER`, `UFFDIO_COPY`,
`UFFDIO_ZEROPAGE`, `UFFDIO_WAKE`, and descriptor `read/poll/ioctl/close`
dispatch; populate pages before completion; and maintain registrations across
unmap, remap, fork, and exec. A faulting guest thread must be tested against a
separate guest handler thread. Until then, this is only reusable plumbing.

Primary protocol reference: [Linux kernel userfaultfd documentation](https://docs.kernel.org/admin-guide/mm/userfaultfd.html).
