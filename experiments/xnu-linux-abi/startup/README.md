# Heap and libc startup on XNU

The isolated `--with-linux-startup` candidate extends the file-operation
checkpoint toward the unchanged Arch systemd version command. It does not
establish a PID 1, service-manager, multithread or physical-hardware pass.

## Current checkpoint status

This is an unproven diagnostic candidate, not a working startup release. The
Linux-reference fixture passes, and the candidate builds. Two live candidates
that replaced the KDK entry object produced a black VM with no boot-test logs;
neither startup nor systemd has passed on XNU. The last working runtime remains
the file-operation checkpoint at commit a52cb73.

Disassembly of the original KDK locore object shows private register-state
handling absent from the public assembly. It also accesses machine offset 0x130,
which the public headers call reserved4. The current TLS experiment uses that
same field and must be replaced before treating this design as safe. The next
implementation must preserve KDK entry code and give Linux its own storage.

A separately built `--startup-entry-mode stock` control retains the original
entry object and is awaiting a live test. Its TLS/x18 fixture is expected to fail;
its purpose is to isolate native boot and the earlier startup backends. The
`source-control` mode compiles the unmodified public entry source to isolate
source/KDK differences. Both controls still contain the experimental machine
state helpers and are not release configurations.

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

Linux AArch64 uses TPIDR_EL0 directly. XNU also uses it for encoded CPU/thread
bookkeeping. The exception-entry hook saves Linux user TLS, restores XNU's
kernel value, and the return hook restores TLS only when returning to marked
Linux EL0. x18 is preserved for the Linux thread. Existing reserved machine
fields 4/5/6 hold TLS, clear-tid address and the Linux signal mask in this pinned
VM experiment; the KDK disassembly demonstrates that these fields cannot be assumed available.
This storage choice is rejected and retained only to reproduce the diagnostic.
No ELF instructions are rewritten. Assembly-generated offsets must match the
working file-operation candidate apart from the new TLS offset definition.

This build normally uses the KDK's prebuilt locore object. The startup candidate
compiles locore from pinned source and uses a private support-archive copy with
only locore.o removed. The tool verifies every remaining archive member is
byte-identical. It leaves the original KDK and previous candidates untouched.
Other native support and driver collections retain their existing provenance.
This changes a sensitive exception path and requires live native/Linux controls.
The current diagnostic is VMAPPLE with BTI disabled; physical kernel-protect
configurations are not qualified by the VM test.

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

## Reproduce

```sh
python3 experiments/xnu-linux-abi/startup/verify.py --linux-reference
python3 experiments/xnu-linux-abi/build_vm_baseline.py --no-lto --no-bti --with-linux-startup
python3 experiments/xnu-linux-abi/link_vm_baseline.py --no-lto --no-bti --with-linux-startup
python3 experiments/xnu-linux-abi/stage_vm_variant.py startup
```

The unchanged ELF compares heap growth/shrink/collision/failure, freed-page
access and regrowth zeroing, resource/random/name operations, signal-mask
roundtrip and actual signal ignore, and TLS/x18 through repeated syscalls and
a forced blocking read. The native parent validates output/status and itself.

The VM boot job also stages the hash-verified original Arch systemd dependency
closure and invokes its original loader explicitly with systemd --version.
It records the result and compares the version output to reference Linux.
Neither reference success nor a build constitutes an XNU runtime pass.
