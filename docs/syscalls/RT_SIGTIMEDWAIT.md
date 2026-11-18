# `rt_sigtimedwait` (137): checked virtual pending-signal subset

The wait now copies the native eight-byte ARM64 signal mask and optional
16-byte timeout from checked guest memory before using them. It writes the
128-byte `siginfo_t` through checked guest memory. If that write fails after a
virtual standard signal is selected, the signal is put back into the pending
bitset and the call returns `EFAULT`. Invalid masks and timeouts no longer
cause a host pointer fault. `SIGKILL` and `SIGSTOP` remain excluded from waits.

The existing queue only models selected standard signals (including parent
death), coalesces duplicates, and is process-wide. A failed copyout followed
by concurrent consumers is not a full Linux per-thread signal transaction.
Realtime queues, arbitrary guest handler delivery and full `siginfo_t` fields
remain unsupported.

Proof: `./build.sh cli` and `python3 tests/run-rt-sigtimedwait.py`. The
disposable ARM64 guest checks invalid pointers, sizes and timeouts, a zero
timeout, and parent-death signal consumption after a failed siginfo copyout.
