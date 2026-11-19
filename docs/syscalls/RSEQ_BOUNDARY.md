# `rseq` (arm64 syscall 293): the single-CPU-0 subset

Shimmy implements registration and unregistration for restartable sequences, but
only the **single CPU 0 / never-in-a-critical-section** subset:

- On register, the guest's `struct rseq` is zeroed (`cpu_id_start=0`, `cpu_id=0`,
  `rseq_cs=0`, `flags=0`, `node_id=0`, `mm_cid=0`), so glibc and allocators see
  "always on CPU 0" and "never inside a critical section".
- The registered area pointer and the per-thread signature are recorded (in the
  thread's `gthread` record, alongside the robust-list registration), and the
  signature is validated on unregister (`RSEQ_FLAG_UNREGISTER`).

What the shim deliberately does **not** do, because it cannot without a real
scheduler/mmu:

- It never intercepts host preemption or migration, so it cannot perform Linux's
  mid-critical-section abort (rewriting the PC and the TLS `rseq_cs` slot). Guest
  code that actually *enters* a restartable critical section would not be aborted
  correctly.

This is safe precisely because the shim advertises "no critical sections ever
active": well-behaved rseq users (glibc's `sched_getcpu` fast path, tcmalloc's
per-CPU caches, jemalloc) read `cpu_id`/`rseq_cs` and, seeing `rseq_cs == 0`,
take their slow paths. No guest code is expected to run a real rseq critical
section under the shim.

If a future revision adds real preemption/migration interception, the abort
(PC/TLS rewrite on migration out of a critical section) can be layered onto the
same `gthread` record without changing the guest-visible registration contract.
