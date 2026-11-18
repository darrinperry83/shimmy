# `rseq` (arm64 syscall 293): scheduler boundary

Shimmy deliberately returns `ENOSYS` for registration and unregistration in
`src/syscalls.c`. That is the truthful result until it can uphold the contract
after a guest thread is preempted, migrated, or interrupted by a signal. A
success response that merely stores a pointer or fills `cpu_id` would let
guest code enter restartable critical sections without a guaranteed abort.
That can corrupt per-CPU data even if ordinary guest threading tests pass.

The current runtime represents guest `CLONE_THREAD` threads as host pthreads
(`src/shim.c:1497-1581`) and keeps guest thread identity by Mach thread port
(`src/shim.c:330-426`). Syscall traps run on a separate exception-server
thread, with a fast syscall path for some calls (`src/shim.c:198-213`). The
shim can see these explicit transitions, but it has no callback for every
host scheduler preemption or CPU migration while guest instructions execute.
Its signal handling also does not implement the full Linux guest signal
delivery path (`src/syscalls.c:5309-5386`). A host migration can therefore
happen between the guest's `cpu_id` check and its final store without Shimmy
rewriting the suspended guest PC to the registered `abort_ip`.

The Linux implementation owns the scheduler and performs exactly that PC and
TLS correction before returning to user mode. See the [kernel rseq source](https://github.com/torvalds/linux/blob/master/kernel/rseq.c)
and [kernel rseq documentation](https://docs.kernel.org/userspace-api/rseq.html).

## Guest proof

`python3 tests/run-rseq-fallback.py --sysroot <Arch arm64 sysroot>` builds a
static arm64 Linux guest. Four guest threads each attempt valid-size aligned
registration and unregistration. They require `ENOSYS` with untouched TLS
storage, then use an atomic fallback concurrently. This verifies the current
negative contract and a usable fallback; it does **not** validate an rseq
implementation or claim all applications have fallbacks. `src/test_syscalls.c`
also checks `rseq -> ENOSYS` at dispatcher level.

## Minimum implementation gate

Before registration can return success, Shimmy needs a per-guest-thread
registration record and an execution mechanism that either detects every
relevant preemption/migration/signal before resume, or prevents them for the
entire critical section. It must validate `rseq_cs`, clear it on abort, set
the guest PC to `abort_ip`, update `cpu_id_start`/`cpu_id` consistently, and
clean registration on thread exit/fork. Tests must force preemption and
migration *inside* the guest critical section and prove no partial per-CPU
commit, including concurrent guest threads and signal delivery. Until then,
no `rseq` success should be exposed, even behind an experimental flag.
