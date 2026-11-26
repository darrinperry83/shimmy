# Arch userspace on XNU with Apple drivers and Darwin applications

**Architecture update — 26 November 2026:** the user selected [XNU serving the
Linux ABI](LINUX-PERSONALITY.md), with XNU owning memory, execution and Apple
devices. Reuse the shim's compatibility contracts in the kernel personality.
The hosted Linux-core prototype remains a separate subsystem experiment; its
memory bootstrap is not a dependency for the shell. Anonymous and private-file
mapping gates now pass on XNU. The immediate application target is the original
Arch systemd package; see [systemd/README.md](systemd/README.md) for gates and
the existing shim reuse map. The unchanged systemd version command now passes
on XNU; Arch Bash and systemd manager/service startup remain unproven.
Keep XNU, personality and Arch package updates independently versioned and
promote tested combinations under [UPDATE-POLICY.md](UPDATE-POLICY.md).

The next file-operation gate also passes on Linux and live XNU; see
[files/README.md](files/README.md). Heap growth, bounded libc/signal startup,
native TLS/x18 preservation and clocks now pass the live startup fixture;
see [startup/README.md](startup/README.md). Native path descriptors, statx and
mount identity now pass as well; systemd --version has empty stderr. See
[paths/README.md](paths/README.md). The next systemd gate is manager
startup and real child-service lifecycle, with required paths/events/process
contracts implemented from recorded failures.


Current plan, 26 November 2026. This is the implementation sequence; STATUS.md
retains the chronological investigation history. Advance a milestone only with
recorded runtime evidence. A successful build, static audit, or mock is not a
runtime compatibility pass.

## Current console checkpoint

Visible output now passes in the disposable VM: an unchanged Arch Linux loader
runs through the kernel Linux ABI, and its stdout/stderr is rendered through the
AppleParavirtDisplay driver without WindowServer. See [console/README.md](console/README.md).
Linux read from a native PTY now passes a scripted input/erase/EOF test on both
Linux and XNU. Next console steps are streaming output and scrolling, Apple HID keyboard input,
a real Linux tty descriptor with read/termios behavior, and then an Arch shell as
the ELF/dynamic-linker syscall subset becomes sufficient. Output proof alone does
not complete any interactive-console or Arch-boot milestone.

## Target and constraints

The next development-environment acceptance target is Codex CLI executing
inside the Arch environment in the disposable VM: inspect/edit the checkout,
run commands and tests, then perform an authenticated online request. Track
process/wait, PTY/signal, libc/TLS, filesystem and networking/HTTPS behavior
needed by the selected CLI build. This is unproven. Keep the existing host
XNU/Apple collection build tools until an in-guest build toolchain is actually
validated. The VM remains a development fixture, not the final runtime design.

- Genuine Arch Linux ARM AArch64 packages and root filesystem.
- Custom XNU owns physical CPUs, scheduling, memory and Apple driver environment.
- Linux ELF processes use a kernel Linux personality, without userspace syscall
  interception, instruction rewriting or a runtime Linux VM.
- Preserve Darwin/Mach-O execution and the Apple support service environment.
- Replace Apple WindowServer and loginwindow implementations while preserving
  required service contracts, companion processes and non-UI session functions.
- Reuse existing shim ABI behavior, tests, broker, rdmetal, scanout and the
  WindowServer replacement. Port code where its assumptions permit; do not treat
  Darwin userspace implementations as directly usable kernel code.
- Physical hardware acceptance is separate from VM acceptance. Full hardware,
  power management, graphics and Darwin GUI compatibility remain unproven.

## Evidence now

DONE: custom VM XNU boots; marked Linux getpid and unknown-call dispatch pass,
ordinary Darwin getpid passes on the same kernel. Repeat boot passes. Exact
kernel/collection identities are recorded in out/vmapple/p0-runtime-result.json.
The M1 static Linux ELF also passes on reference Linux and XNU, with its native
Darwin parent intact; see out/vmapple/elf-runtime-result.json.

LIMITED: WindowServer and loginwindow are absent at the VM process check;
launchd is PID 1 and powerd runs. This does not prove their executables never
ran, physical power behavior, or a complete display replacement. The experimental
launchctl disable overrides are not the final startup architecture.

LIMITED: matching Apple driver collections link and pass static checks. VM
operation does not prove physical devices work. BTI is disabled in the working
VM diagnostic, which must be resolved before a production candidate.

NOT DONE: General Linux ELF compatibility, unmodified Arch application execution,
Arch service boot, broker under the kernel personality, replacement display
startup, general Darwin application compatibility, physical boot.

## M1: minimal Linux ELF checkpoint passed in VM

The bounded static fixture passes on reference Linux and experimental XNU.
Evidence: out/vmapple/elf-runtime-result.json. ELF loading is still minimal;
dynamic Arch executables are the next milestone.

Implement a bounded static AArch64 ELF exec path and explicit process personality.
Start with one static ET_EXEC test, no interpreter or dynamic linker. Validate
ELF headers, segment bounds/alignment, entry point and mappings; establish a
Linux initial stack and ABI state. Reject unsupported images explicitly.

Reuse the shim's ELF validation and syscall tests where applicable. Its gmem/JIT
mapping and executable-instruction rewriting must not enter this kernel path.
Route ordinary Linux SVC #0 by the process personality and x8 syscall number;
remove the P0 x16 marker requirement for ELF processes. Preserve Darwin routing
and correct personality lifecycle on exec/fork/exit.

Only implement getpid, write and exit/exit_group for the first test, returning
Linux -ENOSYS for other calls. Use XNU process, descriptor and copyin mechanisms,
with explicit Linux errno translation; validate bad-pointer and bad-fd behavior.

PASS: execute the same ELF bytes on reference Linux and experimental XNU, compare
output and exit status, verify the PID, -ENOSYS, -EFAULT and -EBADF results, and
run a Darwin control on the same boot. No rewriting or userspace trap handler.
This proves the execution path, not Arch or full Linux compatibility.

## M2 progress: standalone Arch loader command passed

The unchanged Arch Linux ARM glibc loader prints --help and exits 0 on XNU;
its file hash and normalized output match the Linux reference. P0/M1 controls
still pass. See out/vmapple/arch-loader-runtime-result.json. This is a partial
M2 checkpoint: executing /usr/bin/true with its loader/libc, PT_INTERP, file
mapping, heap and thread setup remain unproven. No live guest display exists.

## Following milestones

| Milestone | Required runtime proof |
| --- | --- |
| M2: Arch executable | Run an unmodified package executable with its real Arch loader/glibc. Extend mmap, TLS, signals and file interfaces as demanded by traced behavior; no fake success. |
| M3: process and service semantics | Validate threads/futexes, signals, fork/exec/wait, descriptor inheritance, epoll and required proc/sys interfaces against Linux and existing shim regressions. Run package management and selected Arch services. Keep documented unsupported cases. |
| M4: Apple services and broker | Inventory target jobs and map all display/login endpoints to actual providers. Prove required services and broker run with the kernel personality. Audit non-UI login/session duties. See STARTUP-SERVICES.md. |
| M5: desktop without Apple display servers | Arch compositor presents through rdmetal/scanout and receives working input. Verify frame/fence ownership and pointer behavior. Trace startup and process execution to establish Apple WindowServer/loginwindow never launch. |
| M6: Darwin applications | Keep command-line/Mach IPC regressions throughout M1–M5; then run unmodified GUI applications through the existing replacement and real Apple frameworks. Validate rendering, input and lifecycle, including required service protocols. |
| M7: physical system | Resolve BTI/toolchain support; boot the board-specific kernel/collection in an isolated install. Verify each device, firmware, sleep/wake, thermal/power behavior and recovery. This is not inferred from VM success or powerd presence. |

M4 inventory can proceed alongside M1–M3, but graphics work must not replace the
Linux execution proof. Physical bring-up may expose earlier missing dependencies;
record those rather than marking the whole driver stack supported.

## Rules for each implementation chunk

Keep a working baseline and separate candidate output. Define one observable
acceptance test, record identities and actual results, preserve Darwin controls,
and state what the result does not establish. Update this plan and STATUS.md
before starting a materially different milestone. Do not silently replace the
architecture with a Linux VM or a standalone minimal init that drops Apple
support services.
