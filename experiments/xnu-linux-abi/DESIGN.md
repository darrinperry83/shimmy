# Linux personality in XNU: first experiment

Current checkpoint: the marked P0 entry and a bounded static Linux ELF now pass
on the VM kernel alongside Darwin controls. See elf/README.md and ROADMAP.md.
The design below describes the original P0 entry; it is not full Linux support.

Target: Linux userspace runs directly on an Apple Silicon machine, with Linux
syscalls implemented inside a modified XNU. Retain matching Apple drivers and
the Darwin processes needed by those drivers. No runtime hypervisor and no
userspace syscall interception in this execution path.

Hardware acceptance requires the full stock stack for this Mac, not a reduced
driver subset. See STOCK-HARDWARE-TARGET.md for the current firmware/collection
inventory and the restored SEP base interfaces. See SEP-HIBERNATION.md for the
reconstruction evidence and remaining kernel-wide hibernation limitation. No driver exclusions or fake success implementations satisfy that target.

This changes XNU. It is not stock XNU, even though retaining stock Apple driver
binaries is a goal. Driver compatibility must be demonstrated after a kernel
build and boot; matching source version numbers alone do not establish it.

## Starting point

The existing shim implements a substantial Linux ABI in userspace. It is useful
as a specification and source of regression workloads. It is not linked into
this experiment. Existing desktop, WindowServer, and rdmetal work stays separate
until the kernel syscall path has been demonstrated.

Local machine inspected: M2 Max / T6020, Darwin 25.5.0,
`xnu-12377.121.6~2/RELEASE_ARM64_T6020`. Public source pin:

- Tag: `xnu-12377.121.6`
- Commit: `ac9718fb1af618d5ce8678d0dc6e8a58f252216f`
- Repository: <https://github.com/apple-oss-distributions/xnu>

The matching public source includes T6020 build configuration. The experiment
now builds a complete DEVELOPMENT kernel using the macOS 26.5 SDK and a locally
extracted 25F80 KDK. This is a custom build, not a reproduction of Apple's
shipping kernel binary, and its VMAPPLE variant has now passed the P0 kernel-entry boot test.
Physical T6020 boot remains unproven. See ROADMAP.md for the current sequence.

## P0: prove one kernel entry before an ELF loader

Use a small Mach-O carrier, launched normally by Darwin. It executes:

```asm
mov x8, #172       // Linux AArch64 getpid number
mov x16, #0x4c58   // temporary P0 opt-in marker
svc #0
```

The patched XNU SVC handler recognizes the marker only in a DEVELOPMENT or DEBUG
kernel with `rdxnu_linux_probe=1` in its boot arguments. It reads the Linux syscall
number from x8, obtains the current process ID through XNU's `proc_pid`, and
returns it in x0. Unknown numbers return Linux `-ENOSYS` (-38). The existing
exception-return path resumes the same process after the SVC.

The header leaves all other saved state intact. No PC adjustment, instruction
rewrite, Mach exception server, or userspace handler is involved. Ordinary
unmarked Darwin and Mach calls continue through the original dispatcher. The
marker is not a security boundary: any process on this experimental boot may
request these two harmless results.

P0 deliberately has an extra x16 convention. It is not a Linux process
personality, cannot run an arbitrary ELF, and does not prove Linux userspace or
driver compatibility. The carrier's process creation and exit remain Darwin.

**Kernel acceptance criterion:** on a booted patched kernel, the native probe
reports `KERNEL_LINUX_ABI_P0_PASS`: Linux getpid equals Darwin getpid, unknown
Linux syscall returns -38, and ordinary Darwin getpid still works. Independently
record the kernel build identity and boot log. A test on a synthetic saved state
does not satisfy this criterion.

## Files and local verification

- `rdxnu_linux_probe.h`: actual proposed kernel dispatch code.
- `prepare.py`: content-pinned patcher for `osfmk/arm64/sleh.c`; copies the header
  beside that file. Refuses a different source revision or repeated application.
- `probe.c`, `probe_syscall.S`: native carrier and isolated child-process probe.
  The parent reports failure or capability; it never emulates a syscall.
- `test_dispatch.c`: host model testing the actual header with mocked XNU helpers.
- `verify.py`: compile/model checks, optional upstream patch checks, and optional
  stock-kernel negative control. Imports a matching linked-kernel audit when
  available; boot and kernel-execution claims remain explicitly false.
- `sdk_overlay.py`: creates a disposable SDK overlay with Apple's published
  AvailabilityVersions tool. Does not modify the installed Xcode SDK.
- `dependencies.json`, `build_kernel.py`: pinned sources and KDK, verified local
  package extraction, kernel header/library preparation, and full kernel build.
- `audit_kernel.py`: checks the linked arm64e kernel, unresolved symbols, and
  actual SVC-path disassembly. Records its SHA-256 and UUID.
- `prepare_sep.py`, `kernel/`: restore the missing SEP base interfaces from KDK
  evidence; `audit_sep.py` checks layouts, imports, vtables and generated behavior.
- `preboot_driver_gate.py`: checks all diagnostic and complete stock sets, then
  requires the complete offline collection link and inspection. See DRIVER-SUPPORT.md.
- `link_stock_collection.py`: links all stock drivers without exclusions and
  verifies the exact collection entry set and custom kernel UUID.
- `stage_boot_candidate.py`: copies the collection and reference firmware into a
  local hash-verified bundle, without installing or personalizing boot objects.
- `stock_inventory.py`, `stock_collection.py`: capture the complete loaded-driver
  set, extract and inspect the actual stock boot collection locally, verify the
  board's active FUD payload digests, and inventory on-demand firmware/DriverKit.
  Full-stock loaded and boot driver sets are mandatory in the pre-boot gate.

From the repository root:

```sh
python3 experiments/xnu-linux-abi/verify.py --check-upstream --stock-control
```

On the stock local kernel the marked call terminates the isolated child with
SIGSYS; the parent reports `STOCK_NEGATIVE_CONTROL_PASS`. That proves absence of
the requested entry in the baseline, not success of the replacement.

Prepare an explicit disposable source checkout:

```sh
git clone --depth 1 --branch xnu-12377.121.6 \
  https://github.com/apple-oss-distributions/xnu.git \
  experiments/xnu-linux-abi/out/xnu
python3 experiments/xnu-linux-abi/prepare.py \
  experiments/xnu-linux-abi/out/xnu --apply
```

The source checkout and generated binaries are ignored by git. None of these
commands changes boot configuration, installs a kernel, or modifies WindowServer.

## Build and boot gate

The verified local build uses Xcode's macOS 26.5 SDK. From the repository root:

```sh
python3 experiments/xnu-linux-abi/build_kernel.py --prepare --build
python3 experiments/xnu-linux-abi/audit_kernel.py
```

The script downloads the KDK from the pinned mirror URL, checks its SHA-256,
requires pkgutil to verify signed Apple Software, and extracts both package
payloads under `out/`. It never executes package installer scripts or installs
the KDK systemwide. The user subsequently installed Apple's matching KDK using
Installer so kmutil can discover it for native collection linking. Apple source tags are pinned to the macOS 26.5 release
manifest. The KDK's T6020 release kernel reports the running machine's exact
`xnu-12377.121.6~2` revision.

Resolved build dependencies:

- AvailabilityVersions tool in a disposable SDK overlay.
- Actual KDK private/platform/SPTM headers and CoreEntitlements headers.
- Matching libplatform headers.
- The real libdispatch kernel-exported firehose header, produced using unifdef
  with the upstream target's kernel export defines.
- The real firehose kernel library, compiled with a captured libkern compiler
  argument vector for the same T6020 configuration, then archived with libtool.
- XNU's existing linkage to the KDK's T6020 support library and platform libs.

The experimental handler uses XNU's narrow Mach-to-BSD `proc_pid` declaration;
including the full BSD proc header in the MACH_KERNEL translation unit is invalid.
No fabricated private structures or empty dependency libraries are used.

The build passes explicit SDK/platform/version metadata and real include/library
paths. Bounds-safety instrumentation is disabled (`BOUND_CHECKS=0`) for this
public-toolchain experiment. Compiler warnings remain enabled with `-Wall
-Wextra`; they are not promoted to errors. Upstream warnings remain. The CTF
section is empty because optional CTF-generation tooling has not been supplied;
a dSYM and unstripped kernel are generated. None of this establishes suitability
for deployment or binary driver compatibility.

Output kernel:
`out/xnu/BUILD/obj/DEVELOPMENT_ARM64_T6020/kernel.development.t6020`.
Build command and result are recorded in `out/kernel-build-result.json`; artifact
inspection is in `out/kernel-audit.json` and `out/svc-disassembly.txt`.

The pre-boot driver gate now passes, including the complete 357-entry custom
collection and board-specific firmware digest checks. A local preflight bundle
is staged. Boot encoding/personalization, matching firmware handoff and an
identified alternate boot target with recovery available remain to be established. Asahi/m1n1 being installed gives useful boot infrastructure;
it does not establish an XNU handoff for this kernel and firmware combination.
Do not substitute the Linux U-Boot recipe or the old x86 kernelcache commands.

Apple documents owner-authorized custom XNU boot objects under Permissive
Security, but does not supply or support the custom kernel itself:
<https://support.apple.com/en-lb/guide/security/sec7d92dc49f/web>.
Asahi's macOS-under-m1n1 workflow is a separate hypervisor development route:
<https://asahilinux.org/docs/sw/m1n1-hypervisor/>. A hypervisor test may help debug,
but it does not fulfill the bare-metal acceptance criterion.

Once the patched kernel is actually booted, run:

```sh
experiments/xnu-linux-abi/out/probe --expect-kernel
```

Do not run that command as evidence of success on the stock kernel. No boot or
security settings are changed by the probe itself.

## After P0

1. **Per-process personality:** select Darwin versus Linux at exec, defaulting to
   Darwin; define fork, exec and thread inheritance. Remove the x16 marker from
   Linux processes. Linux dispatch must occur before Darwin interprets x16.
2. **One static ELF:** implement a narrowly checked AArch64 ELF loader and the
   initial Linux stack. Run a libc-free program using getpid, write and exit.
   Implement proper user-memory access, fd/credential checks, and explicit Linux
   errno translation. Do not assume BSD syscall argument structs are compatible.
3. **Linux process semantics:** address spaces, signals, TLS, mmap, futexes,
   clone/exec, fd operations, and procfs. Use existing shim tests as workloads,
   while keeping their userspace implementation out of this path.
4. **Apple driver baseline:** demonstrate the chosen Apple drivers initialize
   under the new kernel; retain their required Darwin services. Graphics and
   Touch ID have additional userspace dependencies. No promise that the latter
   works without Apple's WindowServer has been established.

Systemd and the desktop follow these gates. Suppressing WindowServer is not part
of P0 and is not necessary to prove a Linux syscall serviced inside XNU.
