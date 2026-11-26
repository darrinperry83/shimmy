# Native process lifecycle for Linux ELF

The opt-in `--with-linux-process` variant extends the path checkpoint. XNU owns
process creation, COW maps, descriptor inheritance, credentials, signal delivery,
exec task replacement and child reaping. The existing shim's clone/wait/kill
contracts in `src/syscalls.c` guide the Linux ABI translation; no userspace
broker or parallel process registry is introduced.

The final kernel `65B0EF6B-910A-3305-A3A2-2AD23BDAC70A` passes the unchanged
Linux fixture, native Darwin wait/wide-exit control, original Arch child exec
and all prior regressions. Its console output is visible without WindowServer;
systemd --version still matches Linux with empty stderr. See [evidence.json](evidence.json).

The first fork/wait candidate, kernel 7992801E-D140-3181-9F5F-E1310E589FE1,
passed its unchanged Linux fixture on live XNU. The exec extension uses the
same native process backend with an original Arch executable as its child.

## Fork and inherited state

AArch64 clone (220) currently accepts only SIGCHLD (17), a null child stack,
and null optional arguments. Native fork creates the task and COW mappings.
Before releasing the child, a small hook marks its Linux personality, copies
owned TLS/signal-mask metadata, clears the child's clear-tid address, and gives
it Linux's zero return value. Its x1 argument is preserved rather than exposing
Darwin's child-return flag. Apple machine fields and native assembly offsets
remain unchanged. Heap metadata uses the existing per-process sidecar copy;
parent and child then grow their heaps independently.

## Wait and signals

Waitid (95) supports P_PID/P_ALL, WEXITED, WNOHANG and WNOWAIT. It uses the native
child list, wait synchronization and reap operation. A Linux-only copyout emits
the explicit 128-byte siginfo layout, real child uid and translated signal
status. A valid nonblocking wait with no event clears the output. Failed status
copyout leaves the child available to be waited again.

XNU's ordinary blocking wait saves pointers in a BSD syscall continuation.
Linux SVC arguments live on the current kernel stack, so this variant uses a
native sleep that retains that stack across the wait. Darwin callers retain
the original continuation. This costs one retained kernel stack per blocked
Linux waiter; the systemd event-loop WNOHANG case does not sleep this way.
A native Darwin waitid/WNOWAIT/waitpid control checks the unchanged ABI.

Getppid (173) returns the real XNU parent identity. Kill (129) supports positive
PIDs, signal zero, and the existing bounded Linux-to-Darwin signal map. XNU's
credentials/MAC checks and default/ignore actions remain authoritative.

## Exec and the fixture

Execve (221) delegates to XNU's exec implementation and the existing ELF loader.
The fixture forks and executes the unchanged Arch loader, which loads the
unchanged Arch printf and matching libc. The executable emits
`ARCH_CHILD_EXEC_PASS`; its Linux parent must receive status zero and reap it.
This profile still uses the explicit loader. The opt-in
[interpreter follow-up](../interp/README.md) now proves direct PT_INTERP exec
inside a native chroot without changing this earlier checkpoint. Missing-path and bad-argv failures must preserve TLS.

The fixture also checks independent parent/child data and heap changes,
TLS/x18 and signal-mask inheritance, a forced blocking wait, no-event WNOHANG,
repeated WNOWAIT, EFAULT without losing the event, a single final reap,
ECHILD afterward, Linux eight-bit exit truncation, native Darwin wide exit
status, real SIGUSR1 termination, translated wait status and ESRCH
once the child is gone. The native parent drops the Linux process to uid/gid
65534 when running as root and controls the blocking handshake with a pipe.

```sh
python3 experiments/xnu-linux-abi/process/verify.py --linux-reference
python3 experiments/xnu-linux-abi/build_vm_baseline.py --no-lto --no-bti --with-linux-process
python3 experiments/xnu-linux-abi/link_vm_baseline.py --no-lto --no-bti --with-linux-process
python3 experiments/xnu-linux-abi/stage_vm_variant.py process
python3 experiments/xnu-linux-abi/startup/verify_runtime.py <staged-name>
```

Install only the unique candidate printed by the stager in the disposable VM.
The staged script runs prior regressions, the process fixture, and the exact
systemd version gate. It separately records a manager-start diagnostic; that
attempt is never labelled a manager pass. See [the manager reference](../manager/README.md).

This is a single-thread process checkpoint. Shared-VM/vfork/thread clone modes,
clone3, tid-pointer clone flags, pidfd/process-group waits, stopped/continued
waits, rusage, complete signal frames and PID/init namespaces remain unqualified.
It does not establish a running XNU systemd manager or physical-driver support.
