# `get_robust_list` (AArch64 syscall 100)

This branch gives the shim a per-thread robust-list registration. `set_robust_list`
validates the Linux 24-byte `robust_list_head` size and records the pointer for
the calling guest thread. `get_robust_list` returns that pointer and size for the
current or another live thread in the same shim process, returns `ESRCH` for an
unknown TID, and checks its output addresses before writing. A forked child
starts with no registration. This is tested with `python3 tests/run-get-robust-list.py`.

The focused test is a libc-free AArch64 guest. It covers the initial empty
registration, size validation, round trip, current TID lookup, unknown TID,
invalid output addresses, fork inheritance, distinct parent/child-thread
registrations, and the thread's own query. The test uses a disposable guest root.

**Limit:** robust futex owner-death handling on thread exit is still absent. A
robust mutex whose owner dies will therefore not yet get Linux's `OWNER_DIED`
transition and waiter wakeup. This branch proves syscall 100's query contract
for live threads, not complete robust-mutex compatibility. Cross-process
`get_robust_list` with ptrace permission is also not implemented; unknown
external TIDs return `ESRCH`.
