# Heap and libc startup on XNU

The isolated `--with-linux-startup` candidate extends the file-operation
checkpoint toward the unchanged Arch systemd version command. It does not
establish a PID 1, service-manager, multithread or physical-hardware pass.

## Current checkpoint status

The Linux-reference fixture passes. The earlier public-source entry replacement
produced black VM boots with no test logs; it is preserved in commit 4b9216e for
reproduction and is no longer an available build mode. Disassembly showed that
it omitted private KDK register-state handling and reused machine fields that
Apple actually accesses.

The KDK bridge kernel F82B3332-D5C2-37E0-AED5-BF4381229DE4 passed the live startup
fixture: heap operations, TLS/x18 through repeated syscalls and a blocking read,
real signal ignore, and libc support. All previous native/Linux controls passed;
WindowServer was absent. The stock-entry negative control exited at the expected
TLS stage (116). The unchanged Arch systemd command then reached its clock_gettime
assertion, demonstrating the next missing backend. The clock-enabled follow-up
19096E49-FD42-3D4E-AFC4-95F315780A47 passes the expanded fixture and the unchanged
systemd version command with exact Linux-reference version/feature output and
zero exit status. The console displays that output without WindowServer.
Configuration-path warnings remain on stderr; this is not a manager/PID 1 pass.
See evidence.json for identities, original package/library hashes, and proof logs.

Startup artifacts use names derived from the kernel UUID, fixture binaries, and
staging script to avoid Recovery reusing overwritten host files from its virtiofs
cache. Use the installer filename printed by the stager. Log export discovers
the mounted AppleVirtIOFS filesystem and exports each completed gate.

## Heap and lifecycle

Reuse the shim's `do_brk` return contract: failure returns the previous break,
not -errno. A private process sidecar records the initial/current break and a
per-process mutex. Only page-boundary changes call native XNU allocation or
deallocation. Allocation is fixed without overwrite; collisions preserve the
previous break and existing memory. Shrinking releases pages and regrowth
receives XNU-zeroed pages. No second allocator, mapping ledger or fixed-size
256 MiB reservation is introduced. RLIMIT_DATA bounds heap growth.

The sidecar pointer is appended to XNU-private pstats. Existing proc/task/thread
offsets are retained. Exec resets it; both failed fork construction and normal
exit free it. A copy hook exists for fork, but Linux clone/fork execution and
concurrent fork/heap ordering remain unproven and are not exposed by this gate.
The current RLIMIT_DATA accounting is heap growth only, not a complete Linux
resource-accounting implementation.

## Native TLS and register preservation

Linux AArch64 uses TPIDR_EL0 for userspace TLS. XNU also uses it for encoded
CPU/thread bookkeeping. The bridge saves Linux user TLS before dispatching C
exception handling, restores XNU's kernel value, and restores TLS and x18 when
returning to a Linux EL0 context. EL1 state and Darwin threads delegate to the
original helpers. No ELF instructions are rewritten.

Linux TLS, clear-tid, signal-mask and enable state occupy an owned tail appended
to BSD's uthread. Existing native field offsets and sizeof(struct thread) remain
unchanged. BSD C emits the uthread tail offset; genassym emits the native thread
size. A compile-time assertion pins the relative enable-field offset. Assembly
uses these values rather than assuming that any Apple reserved fields are free.
The combined thread allocation already includes the configured uthread size.

A private copy of the pinned KDK support archive redirects exactly two undefined
helper symbols in locore.o to our wrappers. Instructions, relocation indices,
and all other archive members remain byte-identical. The wrappers invoke Apple's
original state-signing/authentication helpers; other callers retain their original
bindings. Archive/object hashes reject unqualified KDK updates. All existing
assembly definitions must match the working file-operation control. The original
KDK and earlier candidate artifacts are retained.

This sensitive exception-path experiment currently targets VMAPPLE with BTI
disabled. Physical kernel-protect configurations, concurrency, and production
security properties are not qualified by a VM test or successful build.

## Bounded startup syscalls

The candidate adds getrandom, read-only prlimit64 queries, faccessat, thread name
set/get, single-thread gettid/set_tid_address, default/ignore signal actions and
thread signal-mask operations. Linux signal numbers and masks are translated
explicitly. A native parent sends a real SIGUSR1 to validate ignore behavior.
Caught Linux handlers require Linux signal frames/rt_sigreturn and remain
unsupported; real-time signal actions are also deferred. Robust-list and rseq
calls retain ENOSYS rather than reporting invented support. Clear-tid is zeroed
on the current process-exit path, but futex wake/robust cleanup and distinct
thread exit are not proven. Linux thread creation remains unsupported.

The Linux-ignored mmap DENYWRITE and EXECUTABLE bits are decoded without passing
them to Darwin. O_PATH/statx/general fcntl and rootfs namespace work from the
file-operation gate remain incomplete.

## Native clock backend

The clock follow-up implements Linux clock_gettime (113) and clock_getres (114)
for REALTIME (0), MONOTONIC (1), MONOTONIC_RAW (4), and BOOTTIME (7). Calendar
time comes from XNU; uptime uses mach_absolute_time; boottime uses
mach_continuous_time, which includes sleep. XNU owns the boot epoch rather than
rebasing clocks to the launch of a compatibility process. Resolution is derived
from the native timebase. Invalid IDs precede pointer errors; getres permits a
null output, while invalid output addresses return EFAULT.

MONOTONIC and MONOTONIC_RAW currently use the same native awake counter, following
the shim's mapping. Distinct Linux NTP frequency discipline, suspend/resume
qualification, CPU-clock accounting, coarse clocks, and alarm clocks remain
outside this gate. The fixture checks normalized values, nondecreasing uptime,
boottime ordering, resolution, pointer errors, and TLS preservation across calls.
A systemd version pass does not establish complete Linux clock semantics.

## Reproduce

```sh
python3 experiments/xnu-linux-abi/startup/verify.py --linux-reference
python3 experiments/xnu-linux-abi/build_vm_baseline.py --no-lto --no-bti --with-linux-startup
python3 experiments/xnu-linux-abi/link_vm_baseline.py --no-lto --no-bti --with-linux-startup
python3 experiments/xnu-linux-abi/stage_vm_variant.py startup-kdk-bridge
```

The unchanged ELF compares heap growth/shrink/collision/failure, freed-page
access and regrowth zeroing, resource/random/name operations, signal-mask
roundtrip and actual signal ignore, and TLS/x18 through repeated syscalls and
a forced blocking read. The native parent validates output/status and itself.

After boot, use the exact staged name printed by the stager to verify and freeze
runtime evidence:

```sh
python3 experiments/xnu-linux-abi/startup/verify_runtime.py <staged-name>
```

The verifier requires the expected live kernel identity, all prior regression
markers, zero startup/command status, and exact reference stdout. Stderr remains
in separate proof logs and the verification result.

The VM boot job also stages the hash-verified original Arch systemd dependency
closure and invokes its original loader explicitly with systemd --version.
It records the result and compares the version output to reference Linux.
Neither reference success nor a build constitutes an XNU runtime pass.
