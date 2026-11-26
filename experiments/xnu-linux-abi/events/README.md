# Native event counter and bounded poll readiness

The opt-in `--with-linux-events` profile extends the interpreter checkpoint
with Linux eventfd2 (19) and bounded ppoll (73). This is an event-loop
prerequisite, not an epoll implementation or a systemd manager pass.

Live kernel `D8C3FEDC-45FB-3AD3-9380-276D9B151910` passes the unchanged
Linux fixture, all three forced blocking releases, native Darwin event control,
and every earlier gate on the same boot. The console displays the result
through the Apple driver without WindowServer. See [evidence.json](evidence.json).

The existing shim's counter/readiness contracts in `src/net.c`, and the Linux
reference fixture, guide the port. The userspace shim's rendezvous socket and
polling workarounds are not used. A native XNU fileglob owns one mutex-protected
counter. Descriptor references own its lifetime through dup, fork, close and
exec. Native file operations implement read/write, nonblocking mode, semaphore
reads and saturation; native kqueue filters implement read/write readiness.
Reads consume the counter before a destination copy fault, matching the Linux
reference. Writes copy through a duplicate uio so a blocked/interrupted write
cannot falsely report bytes consumed before committing its counter update.

The file type is appended as value 12; existing native values stay unchanged.
The private filter ID is appended after the existing IDs, including DETACHED.
No native fileglob, fileproc, knote, proc or thread structure layout changes.
The KDK entry bridge and earlier native assembly-offset checks remain required.
A fileglob close frees the counter only after native reference teardown. Draining
one fileproc wakes its pending I/O without globally marking dup/fork aliases dead.

Ppoll delegates to native poll, which snapshots the array, registers kqueue
filters and blocks in place rather than entering a BSD syscall continuation.
It uses real readiness wakeups, without periodic rescan threads. Timeout values
are rounded upward to native poll milliseconds, with remaining time copied back
to the raw Linux timespec. Atomic temporary signal masks are explicitly unsupported.
F_GETFL for eventfd returns its actual anonymous-descriptor flags, without the
regular-file O_LARGEFILE bit used by the earlier generic file checkpoint.

```sh
python3 experiments/xnu-linux-abi/events/verify.py --linux-reference
python3 experiments/xnu-linux-abi/build_vm_baseline.py --no-lto --no-bti --with-linux-events
python3 experiments/xnu-linux-abi/link_vm_baseline.py --no-lto --no-bti --with-linux-events
python3 experiments/xnu-linux-abi/stage_vm_variant.py events
python3 experiments/xnu-linux-abi/startup/verify_runtime.py <unique-staged-name>
```

The unchanged Linux fixture checks counter accumulation/reset, semaphore reads,
nonblocking EAGAIN, saturation, invalid counts/flags, read/write copy faults,
eight-byte transfers, dup sharing, fork sharing, close-on-exec and inherited
counter access across a real ELF exec. It checks poll input/output readiness,
closed-fd POLLNVAL, invalid pointers/timespecs and a finite timeout. Three native
supervisor handshakes force separate blocking poll, empty read and saturated
write waits before another process releases them. The native parent also checks
its own pipe readiness; on Darwin it checks native kqueue userdata/data, too.
All earlier fixtures and exact version/direct-exec regressions remain required.

The reference records both immutable Arch package image identity and the actual
Linux kernel version: the container image does not pin its host kernel. The
current reference kernel is 6.8.0-117-generic. This bounded profile follows its
observed transfer contract (writes of at least eight bytes consume eight).
Linux eventfd source is also a useful ABI audit reference:
[fs/eventfd.c](https://github.com/torvalds/linux/blob/v6.18/fs/eventfd.c).
Kernel-version differences must be recorded rather than inferred from an image tag.

Limits: ppoll accepts at most 128 entries, POLLIN/POLLOUT with ordinary error
bits, no temporary signal mask, and a timeout fitting INT_MAX milliseconds.
Nonnegative entries without IN/OUT, priority/extended poll modes and larger
timeouts return explicit errors. Pipe/eventfd readiness is the qualification
scope; general descriptor classes are not established by these tests.
Anonymous fstat metadata returns EOPNOTSUPP until a backed inode view exists.
F_SETFL, complete ioctl/vector-I/O semantics, caught signal frames and concurrent
thread close/drain races remain unqualified. Epoll, timerfd, signalfd, proc/cgroup
views and coherent Linux init lifecycle remain separate systemd work.
