# Arch systemd is the next application target

The target is the original Arch Linux ARM systemd package running through the
XNU Linux personality. Keep its binary and libraries unchanged. The existing
shim is the first source of compatibility behavior and regression cases; do
not restart those implementations from scratch.

## Measurable gates

1. Execute the original Arch loader with systemd's dependency closure and obtain
   the reference `systemd --version` output and exit status on XNU. This is a
   loader/libc startup gate, not a service-manager pass.
2. Run a minimal manager target that starts an unchanged Arch executable,
   receives its exit notification, reaps it and records the correct result.
   Prove timers, signals and descriptor wakeups rather than stubbing success.
3. Establish the Linux init process view and lifecycle: consistent identities,
   parent/child relationships, orphan adoption, signal delivery and wait/reap
   behavior. Reporting getpid()==1 alone does not satisfy this gate. Preserve
   native Darwin support services and their process identities.
4. Boot a minimal Arch system target with a usable console/getty, journal and
   service restart. Expand service coverage from recorded failures.

Systemd's `/proc`, `/sys`, cgroup, mount and event interfaces must report the
behavior actually implemented. Optional unsupported features need truthful
errors so systemd can use its fallbacks. Do not equate every syscall observed
in a reference trace with a mandatory feature.

## Reproducible reference

```sh
python3 experiments/xnu-linux-abi/systemd/reference.py
```

Uses the locally installed Arch image by immutable Docker image ID, with no
network, to copy systemd and its resolved library closure without modification.
It compares the original and copied version command, then traces the copied
command through the original loader with GDB. Docker is only a Linux reference
test tool. No host or guest boot settings are changed.

Outputs in ignored `out/systemd/`: binary bundle, package versions, SHA-256
digests, full syscall entry/return trace and `reference.json`. The trace retains
errors and exit status. Reference success leaves all XNU/systemd runtime gates
false until tested on a matching live kernel. This trace covers `--version`,
not PID 1 startup. Reference Linux and XNU can have different page sizes;
addresses and mapping lengths are not expected to match byte for byte.

The initial reference passes with systemd 262.2-1 and glibc
2.43+r22+g8362e8ce10b2-2. Startup exercises file operations, heap growth,
thread/TLS initialization, signal operations and resource queries beyond the
current XNU filemap checkpoint. `rseq` and other optional calls must be assessed
by their result and caller fallback, not implemented solely to silence a list.

The subsequent [file-operation checkpoint](../files/README.md) now passes on
live XNU, including Linux-opened data files and native metadata conversion.
The reference manifest now identifies syscall numbers that have backends in
that candidate. This does not prove every flag or usage from systemd's startup
trace works; heap/TLS/signal and other startup operations remain unimplemented.

## Reuse and port boundaries

| Area | Existing implementation and regression sources | XNU port responsibility |
| --- | --- | --- |
| ELF and TLS | `src/elfload.c`, `src/guestthread.h`, `src/test_elfload.c` | Native ELF/task/thread state; preserve ABI contracts without instruction rewriting. |
| Heap and mappings | `src/gmem.c`, `src/test_gmem.c`, `src/syscalls.c` (`do_brk`) | XNU task map owns mappings and faults; process lifecycle owns Linux-only metadata. |
| Paths and metadata | `src/syscalls.c` (openat, readlinkat, newfstatat, fstat), `src/syscalls_ext.c` (statx) | Native vnode/descriptor lookup, checked user copies and explicit ABI layouts/flags/errors. |
| Events and sockets | `src/net.c`, `src/signalfd_shim.c`, `src/timerfd_shim.c`, `src/inotify_watch.c` | Reuse readiness/lifetime contracts; replace userspace plumbing with native kernel backends. |
| Process identity and exit | `src/guestthread.h`, `src/pidfd_shim.c`, `tests/test_boot_process_scope.py`, `tests/test_transferable_pidfd_lifetime.c` | Coherent identity, signals, descriptor transfer, fork/exec/exit and wait semantics. |
| Proc and cgroups | `src/syscalls.c`, `tests/guest-cgroup-v2-ops.c`, `tests/guest-cgroup-attach.c` | Back reported membership/state with actual process tracking; expose only supported controls. |

Each port should identify the reused shim contract/tests, the native backend,
and any remaining limitations. Existing shim test results do not automatically
prove the new kernel backend. See [update policy](../UPDATE-POLICY.md).
