# `sysinfo` (ARM64 syscall 179): resource-model blocker

The previous route reported `freeram = totalram / 4`, `procs = 1`, zero swap,
and an eight-GiB total-memory fallback. Those were invented values. Guest
applications can use them to size allocations or worker pools, so the isolated
candidate returns `EOPNOTSUPP` for a non-null result pointer and leaves the
output untouched. A null pointer returns `EFAULT`.

This refusal is **not** full Linux `sysinfo` compatibility. The guest currently
lacks a reliable process count and memory/swap accounting model. The child PID
registry is append-only, so it cannot be used to report a current process
count. Returning Darwin's process count would disclose host state and describe
the wrong operating system. `/proc/meminfo` also carries estimated values and
must be reconciled with any future implementation.

The isolated refusal passed `./build.sh cli` and
`python3 tests/run-sysinfo-refusal.py`, but a disposable Arch boot failed
before login: init exited `-6` shortly after the welcome message. Only the
`sysinfo` route changed in this worktree, so the refusal is a reproducible
boot regression, although the exact init call has not yet been traced. Keep
the runtime prototype isolated. A future implementation needs a coherent
guest memory/process accounting backend and a new private boot gate.
