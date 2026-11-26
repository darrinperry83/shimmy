# Arch + systemd on XNU through a Linux source port

**Direction update:** the user has selected [XNU serving the Linux ABI directly](../xnu-linux-abi/LINUX-PERSONALITY.md),
reusing the existing shim's contracts with XNU owning memory and execution.
This directory preserves the prior source-port/subsystem investigation. Its
independent Linux memory allocator is not a prerequisite for the shell path.

Status: **subsystem prototypes and an experimental XNU host backend; no Linux process port yet**.
Selected by the user on 26 November 2026: no VM; preserve XNU and Apple drivers;
run genuine Arch Linux ARM userspace and systemd; maintain updates from Linux
kernel source rather than extending a syscall-by-syscall Darwin adapter.

The working native ELF/display/PTY checkpoints remain in `../xnu-linux-abi`.
They are execution and hardware-interface controls, not evidence that the new
Linux process port or systemd works. Boot tests use the disposable development VM;
the physical Mac's boot configuration is unchanged.

## Architecture under investigation

```
Linux domain                           Darwin domain
Arch packages / systemd PID 1           launchd / Apple support / Mach-O apps
Linux EL0 processes                    Darwin EL0 processes
         | Linux SVC                            | Mach / BSD entry
         v                                      v
Ported Linux kernel core               XNU BSD / Mach / IOKit
VFS, files, sockets, epoll, futex,
proc, sysfs, cgroups, task semantics
         |                                      |
         +-- hosted architecture interface -----+
         |                                      |
Linux device frontends                 native hardware broker
         +-- versioned device transport --------+
                                                |
                              XNU physical CPU/memory/device ownership
                                   Apple drivers and firmware
```

This is a source-port target, not a stock ARM64 Linux Image loaded into XNU.
An ARM64 Image normally brings its own privileged exception, MMU, interrupt and
CPU initialization. Those paths cannot take over hardware already owned by XNU.
The port must replace the relevant architecture/platform paths. A proposed
`arch/rdxnu` (final layout TBD) would reuse Linux's AArch64 userspace ABI while
using XNU task/thread, mapping, timer and exception mechanisms. That architecture
port does not exist yet. Naming it does not solve process execution or memory.

Linux syscalls should reach Linux implementations and Linux-owned objects.
Avoid a second set of Darwin emulations for files, epoll, sockets, futexes,
cgroups and namespaces. Trap dispatch and user-memory validation still exist;
this removes the intended per-syscall semantic translation, not every interface.
Linux/device protocols still need explicit frontends and host backends.

## Ownership decisions and unresolved blockers

| Resource | Intended owner | Required work / evidence |
| --- | --- | --- |
| Physical CPUs, IRQs, platform power | XNU | Hosted execution must not program privileged ARM64 hardware state. CPU/task scheduling relationship and performance need measurement. |
| Linux task IDs, fd tables, credentials, namespaces | Linux core | Full fork/clone/exec/exit/wait and signal behavior; do not substitute Darwin objects piecemeal. |
| Linux VMAs and application memory policy | Linux core, backed by XNU mappings | **Blocking design work:** one authoritative lifecycle for allocation, faults, COW, protection and teardown, with no divergent Linux/XNU mapping state. |
| Darwin tasks / Mach services / Apple support | XNU / launchd | Preserve native execution and required service contracts. No automatic proof of all drivers or macOS GUI apps. |
| Arch root filesystem | Linux VFS/filesystem drivers | Start with a Linux filesystem image backed by a host block backend; Linux opens files internally. Keep ordinary Linux filesystem syscalls out of a Darwin RPC adapter. |
| Networking | Linux protocol stack | Packet transport to a native XNU interface; define independent ownership and backpressure. |
| GPU/display/audio/input and other Apple devices | XNU drivers + native broker | Linux-facing device interfaces; versioned transport, shared buffers, fences, capabilities and disconnect handling. Reuse existing rdmetal/broker protocols where possible. |

Linux systemd is PID 1 **in the Linux process domain**. The Darwin platform still
has launchd as its own PID 1 and retains Apple support services. These are separate
PID namespaces/domains, not two processes claiming the same global PID and not a
fake return value from getpid. Implementing that distinction is part of the port.
Systemd also needs real cgroup/proc/sys/mount/signal/epoll semantics. A working
Bash prompt or Linux kernel log does not establish a systemd boot.

XNU owns physical scheduling and power management; this does not mean Linux's
logical scheduling or application power behavior automatically matches macOS.
SMP and Linux's scheduler/RCU assumptions are explicit architecture-port gates.

## Reuse candidate: LKL, with explicit limits

LKL compiles Linux code as a library with host operations. It is useful evidence
for moving the boundary below the syscall layer and potentially a starting point
for filesystem/network/subsystem experiments. It is not a ready Arch process host:

- `arch/lkl/include/asm/elf.h` has `elf_check_arch(x) 0`.
- Its MMU `start_thread` implementation in `processor.h` is empty.
- `arch/lkl/kernel/syscalls.c` documents missing userspace signal handling.
- `arch/lkl/Kconfig` depends on `!SMP` in the examined source.

An LKL filesystem/pipe/network test would prove subsystem reuse only. We must
supply native AArch64 user entry/return, address-space isolation, process creation,
signal frames, credentials and SMP before claiming an Arch/systemd runtime.
Do not redirect our ELF processes into LKL's syscall API without solving object
and address-space ownership; that would merely mix two incomplete process models.

Sources examined:
- https://github.com/lkl/linux (architecture and host operations)
- https://github.com/lkl/linux/blob/master/arch/lkl/include/asm/elf.h
- https://github.com/lkl/linux/blob/master/arch/lkl/include/asm/processor.h
- https://github.com/lkl/linux/blob/master/arch/lkl/kernel/syscalls.c
- https://github.com/lkl/linux/blob/master/arch/lkl/Kconfig
- https://docs.kernel.org/arch/arm64/booting.html
- https://docs.kernel.org/process/stable-api-nonsense.html
- https://raw.githubusercontent.com/archlinuxarm/PKGBUILDs/master/core/linux-aarch64/PKGBUILD

## Kernel update design

The concrete maintenance policy is [UPDATES.md](UPDATES.md), with the no-VM
constraint and release gates recorded in [update-policy.json](update-policy.json).
Linux and XNU follow separate source tracks behind a proposed versioned host
interface. The update/release pipeline and interface are not implemented yet.


Arch userspace updates and Linux-core source updates are distinct from Apple/XNU
updates. There is no binary Image-to-XNU conversion. The update input is Linux
source plus the actual Arch recipe/configuration/patch set and our hosted port.
Linux's userspace ABI is intended to remain stable; Linux internal APIs are not.
A small source patch set can reduce update cost, but cannot eliminate rebasing.

Three independently pinned components:

1. **Apple platform:** XNU source, KDK/driver collection, board, native broker.
2. **Linux core:** upstream revision, Arch recipe/config/patch identities, port
   patch revision, host-interface version, compiler/build configuration.
3. **Arch userspace:** root filesystem and package transactions.

Proposed future pipeline (not yet implemented end to end):

```
source/recipe/config intake -> change report -> port patch application
-> configuration migration -> reproducible build -> interface checks
-> Linux reference + XNU differential tests -> Arch shell -> systemd boot
-> candidate package -> explicit activation -> rollback-capable boot test
```

The initial `kernel_intake.py` implements source/recipe/config identity and change
reporting only. It does not apply a nonexistent port, build a Linux core, package,
install or activate a kernel. Every report states those limits. A green intake
report must never be treated as a compatibility pass.

When an actual port passes runtime gates, provide our own Arch package, for
example `rdxnu-linux-core`, from a project repository. Its dependencies must pin a
compatible XNU host-interface range. Ordinary `linux-aarch64` package hooks must
not install a Linux Image as the XNU boot object. Package relationships and
pacman hooks require implementation and upgrade/rollback tests before enabling.
A proposed package name or a `provides=linux` entry does not supply kernel support.
Keep the previous proven XNU + core + matching rootfs snapshot until a candidate
passes boot tests. The activation unit is the compatible set, not one .kc file.

## Proof sequence before resuming broad shell implementation

1. Pin/audit an actual Linux reuse candidate and document its unsupported process
   paths. Keep all current XNU controls bootable.
2. **Native Darwin prototype passed:** boot reused Linux core code with a bounded
   host-operations backend. The custom-XNU kernel backend is still unimplemented. Run Linux
   pipe/epoll or filesystem behavior through Linux implementations. This is not
   an ELF or systemd pass. Begin in a disposable development environment.
3. Define and prove one real isolated AArch64 Linux task: user entry/return,
   copyin/out rejection, mappings and cleanup, with native Darwin controls intact.
4. Prove clone/fork/exec/wait, signals/TLS/futex, memory/COW and SMP. Reuse Linux
   semantics; test against Linux before declaring the hosted backend complete.
5. Run original Arch loader, glibc and Bash from a Linux-owned root filesystem;
   verify external commands, pipes, job control and terminal behavior.
6. Prove mounts, proc/sys, namespaces and cgroup v2, then systemd as Linux-domain
   PID 1 with real services. Preserve launchd support; keep WindowServer absent.
7. Update to a second Linux source revision through the same pipeline. Require
   the same runtime tests and exercise an actual rollback. This is the first
   evidence for maintainable kernel updates, not the initial source inventory.

Hardware broker transport and native console/keyboard improvements can proceed
independently. Do not silently replace this architecture with a Linux VM, claim
stock XNU driver compatibility from a VM, or label the current BSD-backed syscall
checkpoint as the completed source port.

## Local investigation and intake verification

`source-audit.json` pins the examined upstream LKL and native Darwin filesystem
candidate, with file SHA-256 digests. The Darwin candidate's documented build
requires upstream LLVM 19. We have now installed LLVM 19.1.7, built the candidate
natively, and passed Linux pipe/epoll/VFS tests in a Darwin host process; see
[core-smoke](core-smoke/README.md). Its own ELF architecture check still rejects
all binaries. Its fixed filesystem profile is not our process port.

[xnu-host](xnu-host/README.md) now rebuilds that core as a namespaced,
freestanding ARM64e object and supplies XNU host callbacks. The isolated custom
kernel and release-driver collection build and boot; the first core trigger
stalled after the existing ELF/PTY controls passed. Startup tracing is under test.
This is not yet a Linux-core runtime pass inside XNU.

`arch-recipe-lock.json` pins the actual Arch Linux ARM PKGBUILD/config inputs.
`source-intake-example.json` is a real intake of upstream LKL plus those Arch
inputs: it correctly reports their version mismatch and disallows activation.
No upstream recipe or its hooks were executed. All generated reports explicitly
leave conversion, building, systemd and update compatibility unproven.

Usage against a prepared Linux Git checkout (only committed source is examined):

```
python3 experiments/linux-on-xnu/kernel_intake.py --source /path/to/linux --revision <commit> --baseline <previous-commit> --recipe /path/to/PKGBUILD --config /path/to/config --output /path/to/new-manifest.json
python3 experiments/linux-on-xnu/test_kernel_intake.py
```

The four tests cover pinned versus dirty source, actual update path reporting,
non-execution of recipe hooks and wrong-architecture config refusal, and refusing
to overwrite a saved checkpoint. The test also covers empty EXTRAVERSION followed
by another Makefile assignment and recipe/source version mismatch reporting.
The real sparse LKL checkout remains under ignored `out/linux-source`.
