# Native Linux-core reuse checkpoint

Passed on the arm64 Darwin host. The pinned Linux 6.12 LKL/Darwin source builds
into a native Mach-O library. `smoke.c` boots it and invokes Linux's own syscall
implementations as library functions. No VM, CPU emulation, guest instruction
patching or XNU boot change is used in this build/test.

Verified behavior:

- nonblocking Linux pipe read returns Linux EAGAIN;
- Linux epoll is initially empty, observes the pipe payload with its data token,
  and stops reporting readability after the payload is drained;
- pipe EOF after closing the writer, descriptor cleanup and Linux EBADF;
- Linux VFS mkdir/create/write/seek/read/unlink/rmdir with payload checks;
- native Darwin uname/PID remain intact after Linux-core shutdown.

The paths in the filesystem test belong to the Linux core's RAM filesystem,
not to the Mac's root filesystem. The test requires no root privileges.

## Reproduce

Prepare source revision `c4d0895ead9badb52f7bbffe3d529c61c8fc9485` from
https://github.com/HuanchuanTech/lkl (darwin-fskit). The checkout under
`../out/darwin-source` is sparse; it includes tools/lkl, include, arch/lkl and
the directories containing the profile's 714 kernel translation units.
The upstream build requires Homebrew/upstream LLVM 19.1.7 and Xcode tools.

```
python3 experiments/linux-on-xnu/core-smoke/verify.py --source experiments/linux-on-xnu/out/darwin-source --clang /opt/homebrew/opt/llvm@19/bin/clang --build
```

The pinned upstream builder produces an arm64/x86_64 universal archive. Only
arm64 runtime is tested here. The verifier checks the source pin and modifications,
compiles our strict-warning harness, runs it under a timeout, and records source,
archive, executable and runtime-log hashes. Without `--build`, it tests an existing
archive and says explicitly that it did not attest that archive's build provenance.
The successful investigation built the archive separately using the same pinned
upstream builder before running the verifier; `evidence.json` records that fact.

Build/profile validation used upstream `tools/lkl/darwin/test_profile.py`:
seven tests passed. Known case-only netfilter header collisions in the sparse
checkout are recorded, not committed as source patches. The filesystem profile
uses its matching generated host/UAPI headers.

## What this does not establish

The host backend is POSIX threads, mutexes and timers in a native Darwin process.
This is not yet the freestanding backend inside our custom XNU kernel. There is
no Arch ELF process executing through this Linux core, no full shell/systemd,
no isolated Linux user address spaces, no SMP proof and no latest-Arch update
compatibility. A Linux boot log saying `Run /init` is not an executed Arch init.
The profile also reports no kernel memory protection; it must not be used as an
untrusted executable host. Earlier native XNU ELF/console/PTY checkpoints remain
separate controls; do not combine their pass markers into a process-port claim.

## Next implementation boundary

1. Retain the Linux-core smoke as a control while implementing the freestanding
   XNU host backend (threads, synchronization, time, allocation and context hooks).
   Exclude the POSIX/libSystem objects from the kernel artifact. Audit symbol
   ownership/namespacing and linker sections before adding it to a collection.
2. Route one controlled XNU thread into that core and reproduce Linux pipe/VFS
   behavior while the native Darwin controls remain intact.
3. Establish one authoritative process/address-space model, checked user access,
   AArch64 user entry/return, TLS, signals, fork/exec/wait and lifecycle cleanup.
   Do not blindly forward AArch64 syscall numbers into an LKL API: architecture
   argument conventions and the exact UAPI must match. Linux-code reuse does not
   itself establish binary ABI equivalence.
4. Add Linux-owned PTY/device interfaces, a streaming native display/input
   frontend, then run the original Arch loader/libc/shell on those foundations.
   Networking/cgroups/namespaces and systemd follow their own runtime gates.

Reusing the kernel algorithms is now a tested fact. Complete process support and
the XNU kernel integration remain implementation work, not a tested shortcut.
