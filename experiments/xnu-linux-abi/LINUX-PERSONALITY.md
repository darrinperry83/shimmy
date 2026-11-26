# XNU serves the Linux ABI

Selected direction, 26 November 2026: XNU handles the operations requested by
Arch Linux ARM programs. XNU owns task address spaces, actual mappings and page
faults, native threads, physical scheduling, power management and Apple devices.
Linux ELF processes enter an in-kernel Linux personality through SVC #0.
The Arch root filesystem and packages remain Linux userspace.

This follows the user's steering after the hosted Linux-core investigation.
That investigation is preserved in `../linux-on-xnu` as an optional subsystem
experiment. Booting its independent Linux memory/page allocator is not a
prerequisite for the XNU-backed shell path.

## Reuse the existing work

| Existing work | Use in the kernel personality |
| --- | --- |
| `src/gmem.c`, `src/gmem.h`, `src/test_gmem.c` | Reuse the memory contracts, edge cases and W^X experience. Replace userspace allocation, Mach exception and JIT mechanisms with kernel VM operations on the Linux process's task map. |
| `src/syscalls.c`, `src/syscalls_ext.c` and syscall-specific tests/docs | Reuse Linux ABI layouts, flag/errno conversion and behavioral evidence. Audit each native kernel backend; userspace wrappers cannot simply be linked into XNU. |
| `arch-loader/linux_exec.h` and the prepared SVC path | Keep real ELF loading, Linux initial stack and direct kernel entry; extend personality behavior with explicit compatibility tests. |
| `terminal/` and native framebuffer output | Keep the proven PTY/display control while adding actual keyboard routing and a streaming terminal. |
| `rdmetal`, scanout and native brokers | Keep hardware ownership on the Apple/XNU side. Expose the Linux-facing contracts required by applications. |

The authoritative backing map is XNU's `vm_map`. Linux-specific metadata can
record semantics XNU does not already represent, but must not become a second,
divergent account of which memory is mapped or accessible. Memory is reclaimed
through XNU's process/VM lifecycle. File mappings, sharing, COW, faults, fork,
exec and teardown need one coherent owner and explicit tests.

The Linux ABI still needs decoding and compatibility behavior: syscall numbers,
flags, error values, structures, signals, terminal ioctls, TLS and Linux-specific
facilities. Reuse existing implementations and Linux source where useful, with
XNU providing the underlying services. No VM or userspace syscall trap server
is part of the intended runtime.

## First memory gate

`memory/` adds non-fixed anonymous private `mmap`, `mprotect` and `munmap` for
the opt-in Linux ELF personality. They call XNU's native kernel operations on
the current task map. No Linux RAM arena/page allocator is linked into this
variant. Unsupported mapping kinds return an explicit error.

The unchanged Linux ELF fixture tests zero fill, RO/NONE/RW transitions, failed
copyout into read-only memory, failed copyin from inaccessible/unmapped memory,
content preservation, partial unmap, unaffected neighboring memory, repeated
unmap, holes and invalid argument errors. A native parent checks the output,
child status and its own identity. The same fixture is compared on real Linux.
This does not yet prove file mappings, fixed replacement, shared memory, COW,
JIT execution, Linux signals, an Arch shell or systemd.

## Update boundary

This kernel is XNU with Linux ABI support. Arch userspace/package updates and
XNU/kernel-driver updates are separate tracks. A new upstream Arch Linux kernel
image is not an input that can be installed into this architecture. Keep a
tested Linux ABI target, source pins and behavioral regressions so compatibility
changes are reviewable. The earlier source-intake tooling remains useful for
optional Linux subsystem imports, not as an automatic stock-image converter.

Next shell steps: file-backed mappings and heap growth; descriptor/path/file
metadata operations; TLS and thread initialization; signal and terminal
contracts; then original Arch Bash startup and external commands. Each stage
must preserve the current ELF, loader, PTY and Darwin-parent controls.
