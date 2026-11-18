# `getcpu` (ARM64 syscall 168)

The guest's CPU indices follow the host's contiguous CPU numbering, capped at
32 by Shimmy's `/sys` CPU entries, `/proc/self/status`, and
`sched_getaffinity`. `getcpu` now asks Darwin which CPU the calling thread was
running on, checks that it lies in the advertised guest set, and copies the
index to validated guest memory. The guest has one NUMA node, numbered zero.
The legacy cache argument is ignored, as it is by current Linux kernels.

If the host runs the thread on a CPU outside Shimmy's capped guest set, the
call returns `EOPNOTSUPP` rather than inventing a CPU number. The result is a
point-in-time observation; the thread may move immediately afterward. This
does not implement CPU affinity pinning, hotplug, NUMA topology or a guest
scheduler. `/proc/cpuinfo` was capped at the same 32 entries.

Proof: `./build.sh cli` and `python3 tests/run-getcpu.py` with a disposable
static ARM64 guest. The probe checks the returned CPU against the guest
affinity mask, node zero, nullable outputs, and bad pointers.

The synthesized vDSO's `__kernel_getcpu` previously returned CPU zero on
every call even while raw #168 reported the current CPU. It now uses the same
host observation and advertised 0..N-1 bound, with checked guest output
writes. A host query failure returns `ENOSYS` from the vDSO so libc can use
the raw syscall path. `python3 tests/run-getcpu-vdso.py` resolves the exported
symbol directly in an ARM64 guest, checks bad pointers and node zero, and
checks both vDSO and raw syscall observations against the advertised affinity
set. It also requires a nonzero vDSO CPU on this multicore host, catching the
former constant-zero behavior. The thread can migrate between calls, so exact
back-to-back CPU equality is not a sound test.
