# Arch systemd on XNU

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

The [startup checkpoint](../startup/README.md) now runs the unchanged systemd
262.2-1 version command on live XNU kernel 19096E49-FD42-3D4E-AFC4-95F315780A47.
It exits zero and its version/feature text matches the Linux reference. Native
Darwin parent controls and earlier ELF, loader, PTY, memory and file gates pass;
WindowServer is absent. See [runtime evidence](../startup/evidence.json).

This proves gate 1 only. It does not establish PID 1 or a running manager.
The [path follow-up](../paths/README.md) on kernel
4977C3F3-284E-322C-86F7-39C7FA62F17C passes O_PATH, held/unlinked symlinks,
statx and native mount identity. The version command now has empty stderr,
with all prior controls passing. See [path evidence](../paths/evidence.json).
General VFS semantics, procfs descriptor reopening and Arch rootfs isolation
remain work. The runtime verifier requires empty stderr for this profile.
Printed build features describe the package, not implemented kernel facilities.
Heap, bounded signal operations, native TLS/x18 preservation and native clock
backends now exist; caught signal frames, thread creation, futexes, event
interfaces and service lifecycle remain incomplete.

The [process checkpoint](../process/README.md) now proves native fork/COW,
exec of original Arch printf through the original loader, and Linux wait/reap
semantics on XNU, with a native Darwin wait control. The
[minimal manager reference](../manager/README.md) passes successful and failed
services on Linux. XNU's direct manager-start attempt exits at the normal
unbooted-context guard; gate 2 remains unproven on XNU.

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

The [direct-exec checkpoint](../interp/README.md) on kernel
10A390BE-A623-3DF9-8337-20BD4F3C71DD now executes unchanged Arch systemd
by its ordinary `/usr/lib/systemd/systemd` path inside a native chroot.
The kernel resolves PT_INTERP and the Arch loader owns library/TLS setup.
The dynamic probe, original Arch child exec and all previous regression
gates pass. This is a prerequisite for normal service ExecStart paths,
not systemd manager startup. The minimal root's missing-proc warning matches
Linux exactly; the existing outside-root version gate still has empty stderr.

The [native event checkpoint](../events/README.md) now passes eventfd2 and
bounded ppoll through XNU fileglob/kqueue backends on kernel
D8C3FEDC-45FB-3AD3-9380-276D9B151910. Its unchanged Linux fixture forces
blocking readiness, empty-counter read and saturated-counter write wakeups,
and checks dup/fork/exec counter lifetime. Earlier gates also pass on that
boot. Epoll, timerfd, signalfd and coherent init/runtime views remain work;
this does not advance the manager/service acceptance gate.

The [native event-loop batch](../reactor/README.md) now passes on XNU kernel
CF899E7A-3A07-3D2B-A660-0C052D0F00D4: bounded epoll with one-shot rearm,
timerfd, signalfd, and private futex mismatch/timeout/empty-wake controls.
An application linked to the original Arch libsystemd completes the IO,
signal and timer callbacks inside the native chroot, exiting zero with empty
stderr. The first candidate exposed libc's FUTEX_WAKE_PRIVATE initialization
requirement; the native sleep/wake implementation resolves it. Earlier gates
also pass. Exact package hashes, runtime results and unsupported modes are in
[reactor/evidence.json](../reactor/evidence.json). The manager diagnostic remains
at the unbooted-context guard; no PID 1 or service-manager success is claimed.

The next [service-launch checkpoint](../launch/README.md) now passes on XNU
1E73AC27-14B3-3EDD-9F8B-4B31342E0A85. It adds native pipe/dup remapping,
session/group setup, umask, bounded resource-limit mutation and real enforcement,
and wait4 status/rusage. A Linux parent sets up and directly executes unchanged
Arch printf in its native chroot, then verifies exact redirected output and
reaping after a forced peer release. All earlier gates pass. These controls
are prerequisites for service executors; the manager/init-context boundary is
unchanged. See [launch/evidence.json](../launch/evidence.json).
