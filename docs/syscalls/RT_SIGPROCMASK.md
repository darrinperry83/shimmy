# `rt_sigprocmask` (135): checked process-wide mask subset

This increment requires the native ARM64 eight-byte `sigsetsize`, stages a
guest input mask through checked memory access, and writes the old mask through
checked output before changing state. An invalid input or output pointer cannot
silently change the mask. An invalid `how` with a non-null input is rejected,
and Linux SIGKILL/SIGSTOP bits remain unblockable. A null input queries the
current mask, including when `how` is otherwise irrelevant.

The existing virtual mask is **process-wide**. Linux masks are per-thread, and
Shimmy does not deliver arbitrary guest signal handlers, realtime queues or
atomic mask changes around every blocking syscall. This is a checked subset,
not full signal compatibility.

Proof: `./build.sh cli` and `python3 tests/run-rt-sigprocmask.py`. The disposable
ARM64 guest checks size, bad pointers without state change, BLOCK, SETMASK,
UNBLOCK, old-mask output, and the unmaskable bits.
