# `process_mrelease` (ARM64 syscall 448): early memory reclaim blocker

Linux's `process_mrelease(pidfd, flags)` takes a pidfd for an exiting task and reaps its anonymous address space before ordinary process teardown. The kernel rejects nonzero flags, resolves the pidfd, finds a task that still owns an `mm`, checks whether the task is freeing memory or has already been reaped, and may return `ESRCH`, `EINVAL`, `EINTR`, or `EAGAIN`. Successful return means the early reclaim operation ran or had already completed.

Shimmy's pidfds identify Darwin processes through `pidfd_shim.c`. That registry can support identity, signaling, and wait operations, but it does not expose a dying process's address-space map or the Linux `MMF_OOM_SKIP`/OOM reaper lifecycle. Darwin will reclaim memory when the host process exits; that is later than the operation this syscall promises. Calling `kill`, `waitid`, or `madvise` on the caller's mappings cannot substitute for it. Returning success without an early reclaim would mislead an OOM manager.

Keep the existing `ENOSYS` fallback. A real implementation would need explicit per-guest memory-map ownership, a safe cross-process reclaim operation during exit, and pidfd lifecycle synchronization. Validate it with a disposable ARM64 Linux oracle and guest tests that distinguish a live target, an exiting target, an already reaped target, invalid flags, and invalid/stale pidfds. Do not test this by reclaiming the live desktop or host process.

Primary source: [Linux `mm/oom_kill.c`, `SYSCALL_DEFINE2(process_mrelease)`](https://github.com/torvalds/linux/blob/master/mm/oom_kill.c#L1112-L1172).
