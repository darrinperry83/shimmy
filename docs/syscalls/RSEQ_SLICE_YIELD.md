# `rseq_slice_yield` (arm64 syscall 471): no-grant result

The installed Arch arm64 `asm-generic/unistd.h` assigns number 471 to
`rseq_slice_yield`. The Linux kernel [source](https://github.com/torvalds/linux/blob/master/kernel/rseq.c)
returns `1` only when a thread successfully yields a granted rseq scheduler
slice, and `0` when no slice was granted or it was revoked. This call takes no
arguments.

Shimmy currently returns `ENOSYS` for `rseq(293)`, so a guest cannot register
an rseq critical section or receive a slice grant. The new 471 handler returns
only `0`, without claiming a yield, touching guest memory, or altering host
scheduling. It does **not** implement rseq registration, preemption aborts,
slice extensions, or a scheduling guarantee. Callers must not infer full rseq
support from this zero result; 293 remains the registration gate.

`python3 tests/run-rseq-slice-yield.py` builds a freestanding ARM64 guest and
checks that 293 still returns `ENOSYS`, then calls 471 repeatedly with varied
unused argument registers. `./build.sh cli` and this probe passed in the
isolated branch. A private Arch boot regression also passed: the disposable
rootfs reached a fresh login prompt, and the D-Bus broker and launcher settled.
The scoped guest process and clone were cleaned afterward.
