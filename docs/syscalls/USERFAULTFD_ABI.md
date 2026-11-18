# `userfaultfd` Linux ABI adapter, still off dispatch

This branch adds `src/uffd_linux_abi.c/h` beside the private event backend.
It validates a minimal Linux `UFFD_API` request, accepts only aligned
anonymous `UFFDIO_REGISTER_MODE_MISSING` ranges after a caller-supplied
eligibility check, delegates registration/unregistration to the backend, and
encodes a queued page-fault event as a Linux `uffd_msg`. The wire structure,
offsets, version, mode, and event number are checked against the installed
Arch `linux/userfaultfd.h` in `tests/run-uffd-linux-abi.sh`.

The adapter is deliberately **not linked into `shim_cli` or `shim_gui`** and
does not handle a guest `ioctl`. `userfaultfd` syscall 282 remains `ENOSYS`.
Its positive registration test supplies a synthetic eligibility callback;
production has no callback that can prove a guest anonymous page was still
unpopulated when registration occurred. Returning a descriptor before that
check, a real Mach fault relay, page population, Linux `UFFDIO_COPY`/
`UFFDIO_ZEROPAGE`/`UFFDIO_WAKE`, and read/poll/ioctl/close routing would
promise a paging facility that cannot work.

`sh tests/run-uffd-linux-abi.sh` passes a synthetic reporter through the
private pollable queue, event dequeue, Linux wire encoding, and completion,
then checks invalid API features, unsupported mode, eligibility rejection,
alignment, overlap, and unregister. `sh tests/run-uffd-backend.sh` still passes.
Neither test is a real guest page-fault test.

The existing Mach `EXC_BAD_ACCESS` handler in `src/shim.c` uses faults for
guest JIT W^X and must keep first chance for its registered JIT ranges.
A new UFFD path needs a separate guest mapping/residency record and must not
mistake arbitrary invalid pointers or JIT protection transitions for missing
pages. Only after an ARM64 guest test faults, reads a Linux event, copies or
zeroes a page, and resumes may syscall 282 be enabled.
