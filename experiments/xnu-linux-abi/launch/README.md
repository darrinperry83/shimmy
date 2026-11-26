# Native service launch controls

The opt-in `--with-linux-launch` profile extends the event-loop checkpoint with
service process setup. XNU continues to own file tables, pipes, process groups,
sessions, resource limits, accounting and child reaping. The existing shim's
flag, descriptor and rusage contracts inform this implementation; its guest
PID registry and userspace memory plumbing are not linked into the kernel.

This batch now passes on live custom XNU kernel
`1E73AC27-14B3-3EDD-9F8B-4B31342E0A85`, together with every earlier regression
gate and original libsystemd's event loop, with WindowServer absent. See
[evidence.json](evidence.json) for exact identities, output and qualification.

## Implemented scope

* `pipe2`, `dup`, `dup3`, `F_DUPFD_CLOEXEC` and bounded `F_SETFL` provide native
  pipes and standard-output remapping. CLOEXEC and NONBLOCK are translated,
  aliases share the native fileglob, and failed pipe copyout closes both fds.
  Pipe creation and dup3 publication require one native thread. This is an
  explicit boundary until atomic flag publication is implemented for threads.
* `setpgid`, `getpgid`, `getsid` and `setsid` use actual native session/group
  membership and permission checks. No synthetic Linux PID/session identity is
  introduced. Group-directed kill remains unsupported under the earlier gate.
* `umask` changes the native file creation mask. The fixture creates an actual
  file with mode 0666 under mask 027 and requires mode 0640; fork inheritance
  and parent independence are also checked.
* `getrlimit`, bounded `setrlimit`, and `prlimit64` use native limit storage and
  enforcement. Mutation currently covers CORE and NOFILE in the current process;
  earlier read-only resource mappings remain. The fixture lowers a child's soft
  and hard NOFILE limit, requires descriptor-limit enforcement and failed hard
  limit elevation, and verifies the parent's limit is unchanged. Infinity is
  translated rather than passed as an unchecked native value. No reporting clamp
  replaces actual enforcement.
* `wait4` supports exited children and WNOHANG with native selectors/reaping.
  Linux signal numbers and maxrss in KiB are emitted; zeroed native timeval
  padding preserves the AArch64 64-bit timeval representation. Blocking waits
  retain the kernel stack rather than entering a BSD syscall continuation.
  Stopped/continued waits and comprehensive output-fault/reap races remain
  unqualified. Native Darwin wait/status/rusage behavior stays on its native path.

The original Arch printf executable is started by ordinary PT_INTERP exec in
a native chroot after session creation, stdout remapping, fork, a forced peer
release and another CLOEXEC-checking Linux exec. The Linux parent verifies exact
pipe output, zero child status and wait accounting. A native supervisor holds
two separate releases for 200 ms and requires the Linux parent to remain alive
while blocked. The same ELF and Arch package bytes run in the Linux reference.

## Reproduce

```sh
python3 experiments/xnu-linux-abi/launch/verify.py
python3 experiments/xnu-linux-abi/build_vm_baseline.py --no-lto --no-bti --with-linux-launch
python3 experiments/xnu-linux-abi/link_vm_baseline.py --no-lto --no-bti --with-linux-launch
python3 experiments/xnu-linux-abi/stage_vm_variant.py launch
python3 experiments/xnu-linux-abi/startup/verify_runtime.py <unique-staged-name>
```

The raw fixture, native supervisor and original Arch printf/loader/libc closure
are staged by hash. Test files are created in distinct owned work directories;
they are excluded from the package closure. Every earlier regression gate runs
on the same boot, including original libsystemd's event loop.

## Manager boundary

This is service-launch infrastructure, not systemd PID 1 or a running manager.
Systemd's [startup safety checks](https://github.com/systemd/systemd/blob/v262.2/src/core/main.c)
require a real system-manager init context or an already-booted system for the
user manager. A `/run/systemd/system` marker alone cannot provide process
adoption, init lifecycle, proc/cgroup/rootfs views or manager service behavior.
Those remain explicit work, together with caught Linux signals, complete
threading, general service isolation and interactive shell/input support.
