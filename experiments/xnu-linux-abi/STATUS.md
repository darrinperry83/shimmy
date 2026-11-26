# Status — current checkpoint 26 November 2026

Selected direction: [XNU serves Arch's Linux ABI](LINUX-PERSONALITY.md), following
the user's explicit decision that XNU should handle memory and other operations.
The existing shim supplies substantial compatibility contracts to reuse. The
new isolated `--with-linux-memory` variant uses XNU's task map for Linux ELF
memory operations; the same ELF now passes on real Linux and live custom XNU,
and its result is visible through the Apple display output host. This is the
active route toward the shell. Evidence: [memory/evidence.json](memory/evidence.json).

The next private-file-mapping gate also passes on real Linux and live XNU
(`238B1961-DF68-30ED-9368-DD2E7B7C35D3`): private data with an unchanged backing
file, execution from an RX file mapping, offsets, fixed file/anonymous
replacement with intact neighboring memory, and Linux error behavior. Native
Darwin's directory-mapping error remains unchanged. All earlier controls pass,
and the result is visible without WindowServer. Evidence:
[memory/filemap-evidence.json](memory/filemap-evidence.json). Later file and
startup gates below extend this checkpoint; Bash remains unproven.

The file-open/metadata checkpoint now passes on live XNU kernel
`2A795DD5-D68C-30A6-84B0-7237329792AC`: the unchanged Linux fixture opens its
own data file, checks Linux metadata against a native Darwin parent, exercises
path/symlink/fd/error behavior and 64-bit offsets, then executes a file mapping
after closing its descriptor. The result is visible through the Apple display
path. Earlier ELF, Arch loader, PTY and memory/filemap controls pass on the same
boot with WindowServer absent. See [files/README.md](files/README.md) and
[files/evidence.json](files/evidence.json). This uses the native filesystem
namespace; Arch rootfs policy and complete manager/service behavior remain work.

The original Arch systemd 262.2-1 version command now executes on XNU kernel
`19096E49-FD42-3D4E-AFC4-95F315780A47`, exits zero, and prints the same version
and feature lines as the Linux reference. Native and previous Linux regression
gates pass; WindowServer is absent. The startup fixture also passes heap,
TLS/x18 through repeated syscalls and a blocking read, signal ignore, bounded
libc support, and clocks. Evidence: [startup/evidence.json](startup/evidence.json).

The TLS bridge preserves the KDK entry instructions and redirects two helper
imports in a private archive copy. Linux metadata has owned uthread tail storage;
Apple's private machine fields and state-authentication helpers remain intact.
The earlier public-source entry replacement failed native boot; the stock-entry
control passed earlier gates and failed exactly at the TLS check.

This is a loader/libc application gate, not PID 1 or a service-manager pass.
The path follow-up on kernel `4977C3F3-284E-322C-86F7-39C7FA62F17C` now
passes native O_PATH descriptors, held/unlinked symlinks and inodes, bounded
fcntl, basic statx and native mount identity. The unchanged systemd version
command matches the Linux reference and has empty stderr; all earlier controls
pass with WindowServer absent. See [paths/README.md](paths/README.md) and
[paths/evidence.json](paths/evidence.json). Threading,
caught Linux signals, futex/event semantics, coherent init lifecycle, rootfs
isolation and interactive hardware input remain work. See
[systemd/README.md](systemd/README.md) and keep XNU, personality and Arch updates
independently versioned under [UPDATE-POLICY.md](UPDATE-POLICY.md).

The native process checkpoint now passes on kernel
`65B0EF6B-910A-3305-A3A2-2AD23BDAC70A`: Linux fork/COW, independent heap/TLS,
real signal termination, blocking/nonblocking waitid, WNOWAIT and exact reap,
plus Linux exec of the unchanged Arch loader/printf. Linux exit status is
correctly truncated to eight bits; a native Darwin wide-exit/wait control passes.
All prior checks and clean systemd --version pass on the same boot. See
[process/README.md](process/README.md) and [process/evidence.json](process/evidence.json).

The [manager reference](manager/README.md) runs real Arch systemd as Linux
container PID 1 and verifies both successful and failed children. The XNU
manager attempt stops at systemd's unbooted-context guard, exactly as its Linux
control does. A coherent Linux init/runtime context and manager event interfaces
remain work; no XNU manager or PID 1 pass is claimed.

The earlier [Linux source-port investigation](../linux-on-xnu/README.md) is
preserved as an optional subsystem experiment rather than the shell dependency.
Source candidates and update inputs are pinned and audited; the intake tool passes
four tests and a real-source run. A pinned Linux core has now been built as native
Mach-O and passed pipe/epoll/VFS tests in a Darwin host process without a VM; see
[core-smoke](../linux-on-xnu/core-smoke/README.md). An experimental
[XNU host backend](../linux-on-xnu/xnu-host/README.md) now builds and links the
freestanding ARM64e Linux core into a separate custom kernel. That kernel boots
and passes the existing controls; its first Linux-core trigger stalled and
startup tracing is under test. In that separate Linux-core variant, Arch
process support, systemd and a converted Arch kernel remain unimplemented. Existing VM execution/display/
PTY evidence below remains valid. Reuse its kernel personality and the shim's
tested ABI contracts with native XNU backends.


Current implementation sequence: [ROADMAP.md](ROADMAP.md). The kernel-entry
probe and minimal static Linux ELF execution now pass in the VM; Arch boot
remains unproven. Visible Linux output now passes; input is next. The sections below retain earlier investigation states and are chronological.

The driver pre-boot gate passes. The custom DEVELOPMENT T6020 kernel builds,
and Apple's linker accepts it with the complete stock driver set. The linked
collection has exactly the stock collection's 357 entry IDs and our kernel UUID.
It is not installed, encoded/personalized for a new boot identity, or booted.

## Linux terminal input checkpoint — 26 November 2026

Console/ELF foundation is published as `bb4af5d` on development. The next isolated
VM candidate adds Linux read through XNU's descriptor/tty path and boots as UUID
`713AB341-1CF9-3E1A-8601-E2D8F75E1EAF`. The unchanged Linux ELF input fixture passes
on real Linux and XNU: bad descriptor, bad pointer, zero count, canonical erase,
and Ctrl-D EOF. The native PTY parent survives and requires zero child status.
P0/M1/Arch-loader regressions pass, and the PTY transcript is visible without
WindowServer. Evidence: `terminal/evidence.json` and `out/vmapple/results/terminal-boot.log`.

Input is scripted through a native PTY. Physical keyboard routing, streaming/VT
rendering, Linux terminal ioctl translation, and Arch Bash remain unfinished.
`terminal/bash-reference.json` records the original Arch Bash package and observed
startup/builtin syscalls to guide subsequent implementation; Bash has not run on
XNU. The bitmap host still does not interpret echo control characters.

## Visible Linux output checkpoint — 26 November 2026

The VM now visibly displays the original Arch glibc loader's `--help` output,
executed as Linux ELF on custom XNU UUID `7BB526DA-C8F2-3E30-A7F0-F5EAB18461D4`.
A native Darwin renderer captures the child's actual stdout/stderr and presents
its own IOSurface directly through AppleParavirtDisplay's IOMobileFramebuffer
user client. Child exit status and SwapBegin/SwapSetLayer/SwapEnd all return zero;
the 2048×1536 guest screen was visually verified. The no-WindowServer profile is
retained; P0, M1 and Arch-loader checks pass. Evidence is in
`out/vmapple/console-runtime-result.json` and `results/console-display-proof.log`.

This is output only: no Linux tty, keyboard input, interactive shell, dynamic Arch
application or Arch boot is proven. The legacy XNU console overlay builds and
boots but writes to a different 640×1136 framebuffer; it did not update the screen.
The direct display proof is `console/display_output.c`. Both pinned submodules
are initialized; existing scanout injects into WindowServer and was not used.

## Implemented and verified

- Linux AArch64 getpid/negative-ENOSYS kernel-entry experiment, boot-gated and
  separate from the existing userspace shim. The native carrier and dispatch
  tests pass. Stock XNU rejects the marked probe with SIGSYS as expected.
- Complete XNU build with the matching KDK and real Apple source dependencies.
- Reconstructed SEP base classes restore all 16 missing driver imports. Object
  sizes, field offsets, class/metaclass vtables, PAC and selected generated
  behavior match the KDK evidence. See SEP-HIBERNATION.md.
- Full-stock inventory: 256 loaded Apple components and 357 active stock boot
  collection entries; loaded non-KPI driver UUIDs match the reference.
- Platform, input, storage, GPU/display, media/camera and built-in/AOP/USB audio
  diagnostic profiles have no unresolved imports or binary inspection errors.
- Whole-stock scans have no additional unresolved imports versus Apple's raw
  control. Four battery/RAID imports remain reported in both raw controls. They
  are adjudicated by the successful complete kmutil link, not an allowlist.
- Sixteen vtables / 1,677 actual method slots match. Alignment padding is excluded.
- All 15 board-specific active FUD payload digests match the pinned restore
  manifest. The shared SEP image, 3,549 on-demand firmware files and 22 installed
  DriverKit extensions are inventoried. Device-specific runtime mapping is not
  established.
- Kernel, firmware-parser, missing-dependency/export, slot-order and PAC negative
  controls pass.

The earlier GPU report was a checker bug: flat-layout IOGraphicsFamily provides
those imports. Its executable lookup is corrected and regression-tested.

## Artifacts

Kernel SHA-256:
`ad8ec373a275727b1ad5aca21634dd32aa2bbd13243e4cadd07d2b66970c33c8`.
Kernel UUID: `FFBA4F2E-9DE0-378A-9C54-EE95809D9236`.
Collection SHA-256:
`597a85954310e67298a9dead1272d0af44a7876984d0c6faa1fe5e26fb2f5c5f`.

The linked collection is out/driver-check/full-stock.kc. Its command, complete
contents and result are recorded beside it. A local preflight bundle is staged
at out/boot-candidates/597a85954310e672: 20 copied files verified by hash, covering
the custom collection, unchanged stock FUD inputs, device tree and shared SEP
reference. The manifest and README explicitly state that this is not an
installed or newly personalized boot object. On-demand firmware, DriverKit and
required Darwin services must remain available on the matching stock root.

The matching Apple KDK is now installed in /Library/Developer/KDKs; both Apple
package receipts were observed. The initial kernel is preserved under
out/baseline-before-sep. All generated artifacts remain ignored by git.

## Remaining work

Boot encoding/personalization, selection of an alternate boot target, matching
firmware handoff and the loader route remain to be established. No custom
kernel has executed on hardware. ELF loading, Linux syscall execution in the
new kernel, device initialization, and complete hardware functionality remain
unproven. Kernel-wide hibernation remains disabled in the public configuration;
the new SEP interfaces do not establish full hibernate/restore support.

No startup disk, boot policy, system volume, WindowServer job or existing shim
implementation was changed. No firmware was flashed. Nothing committed/pushed.

## Commands

    python3 experiments/xnu-linux-abi/build_kernel.py --prepare --build
    python3 experiments/xnu-linux-abi/preboot_driver_gate.py
    python3 experiments/xnu-linux-abi/stage_boot_candidate.py

The future hardware criterion remains KERNEL_LINUX_ABI_P0_PASS from the native
probe on the actual experimental boot, with its kernel identity recorded.

## VM test preparation

A separate DEVELOPMENT ARM64 VMAPPLE kernel builds with the same P0 patch.
Its linked syscall audit and 12 core IOKit vtables (1,297 slots) pass against
Apple's VMAPPLE control. UUID: B4BDC5FA-9643-3732-B891-802487AFE5DB.
The physical T6020 pre-boot gate still passes after the helper changes.
See VM-TESTING.md for the disposable 25F80 guest and runtime acceptance test.
The VM restore's 218-entry collection also links with the custom kernel and
passes exact IDs, kernel UUID and kmutil inspection. The source IM4P digest
matches the VM restore manifest. The transfer disk's copied contents pass hash
checks. The full IPSW SHA-256 matches Apple's CDN digest. The VM member and build also
match the completed archive. Apple VM installation succeeded, and the stock
25F80 guest boots to Setup Assistant. A 4-CPU / 8-GB / 64-GB disposable VM named
rdxnu-25f80 is running in Tart with the transfer disk and a results-only share.
The account name is prepared as rdxnu; no password has been entered. First-boot
account creation requires the user to complete credential entry under the
Computer Use tool's confirmation policy. No custom kernel has booted in the VM,
and no guest boot-security policy has been changed.

## Stock guest runtime control — 26 November 2026

The user completed first-boot setup. The stock 25F80 VMAPPLE kernel's UUID
matches the KDK, all 118 loaded components match the restore collection, and
the native marked syscall is rejected with SIGSYS. GUEST_STOCK_BASELINE_PASS.
Evidence: out/vmapple/stock-runtime.json and results/stock-baseline.log.
The guest was shut down cleanly and cloned as rdxnu-25f80-stock.

The working VM is in recoveryOS at its rdxnu owner password prompt. User
recovery authentication is pending. No SIP/boot-policy change has been made,
and the custom kernel has not executed. guest_recovery.sh is prepared with
VM-only, build and collection-hash guards; its configure mode has not run.

Recovery authentication was attempted with the user-supplied guest credential.
Recovery rejected the entry and displayed a temporary account lockout. Further
attempts were stopped while it clears. The stock checkpoint remains available;
no credential reset or guest/host boot-policy change has been performed.

## VM recovery policy and current blocker

Recovery owner authentication completed, and SIP was disabled in the VM.
The user explicitly authorized arbitrary changes to this disposable guest.
bputil reports Permissive Security, disabled kernel CTRR, disabled boot-argument
filtering, and an absent custom-KC hash (coih). The physical host is unchanged.

Recovery lacks shasum and openssl. A small native CommonCrypto SHA-256 utility
was built and checked against an empty vector and the actual collection; the
recovery helper then passed the collection hash and build guards. kmutil accepts
and wraps the collection but rejects owner credentials while updating policy.
A native PTY also reproduced that rejection; no custom boot installation or
kernel execution has been verified. Credential clarification is pending, and
further password attempts are stopped. The stock checkpoint remains intact.

## VM installation completed; startup still unresolved

The user corrected the guest credential. Owner authentication now passes;
kmutil installed the boot object and bputil confirmed a present coih hash.
The native SHA-256 helper passed the actual collection hash check in Recovery.
No credential is embedded in the source files.

Recovery refuses unqualified boot-args writes. Using the explicit Apple system
GUID succeeds and reads back the probe, verbose, unsafe-kernel-text, keepsyms
and debug boot arguments. guest_recovery.sh now uses that namespace.

A retry still produced a black display, with zero bytes captured through Tart's
serial device. Custom kernel execution and the Linux syscall runtime probe
remain unverified. The guest was returned to recovery options for continued
diagnosis; the stock checkpoint and physical host boot policy are unchanged.

## Controlled boot test — 26 November 2026

The development collection failed to reach a visible boot in both uncompressed
and compressed attempts with serial=3 and debug=0x108. Both remained black;
no custom running kernel identity or P0 probe result was obtained.

An Apple KDK RELEASE VMAPPLE control collection with the same 218 identifier
set boots to the desktop through the compressed custom-boot path. Runtime
kern.kernelcacheuuid equals 551E9C99-1EEA-09B9-6101-80C7A541CC9A, matching the
rebuilt control collection exactly. kern.uuid equals
8012A15E-57F1-32BD-903A-48F2332790BB. The boot-argument readback includes the
probe, unsafe-kernel-text, keepsyms, verbose, debug=0x108 and serial=3 settings.

The control uses release driver variants; the failed collection uses development
variants. The test isolates the problem to the development collection/build,
but does not distinguish kernel code from driver variant or layout differences.
Serial capture is empty even in the successful control, so it does not locate
the failure before kernel entry or serial setup. No panic report was found
under /Library/Logs/DiagnosticReports.

The VM is left logged in on the working Apple control desktop with Terminal
open. The stock checkpoint and physical host boot policy are unchanged.
Evidence: out/vmapple/boot-test-result.json, results/control-runtime.log and
results/control-identity.log. This is not a Linux ABI runtime pass.

## SVC-unpatched baseline test — 26 November 2026

build_vm_baseline.py built DEVELOPMENT_ARM64_VMAPPLE into isolated output
directories with the original pinned sleh.c. The experimental SVC source was
restored in a finally block under the shared build lock, and the earlier patched
kernel hash is unchanged. The baseline binary has no rdxnu_linux_probe string.
Other prepared build-support changes are retained; this is not a claim of an
entirely untouched Apple build environment.

link_vm_baseline.py linked this kernel with release driver variants. All 218
identifiers and every non-kernel driver UUID match the working Apple control.
Baseline kernel UUID: 5175363A-7C27-3BD6-9176-FC7DFA9D07B7.
Collection UUID: B088C8C7-0854-A93F-241C-3EC97D626460.
Collection SHA-256: 58632046e865cd9caf7a55abc6003da5486fa558863f031ee5524d48640fa765.

The guarded Recovery installer passed the collection hash check, installed the
compressed baseline, and read back the boot arguments. The system-disk boot
remained black for approximately two minutes and did not answer two ICMP probes.
No running baseline UUID or runtime pass was obtained. Thus the failure
reproduces without the SVC patch and without development driver variants. This
focuses diagnosis on the source-built kernel, configuration, supporting code or
its resulting collection, rather than establishing the syscall hook as the cause.
Evidence: out/vmapple/baseline/build-result.json, collection-result.json and
boot-test-result.json. The working Apple control is being restored afterward.

After the baseline test, the Apple control collection was restored and booted
back to its login screen successfully. No host boot policy was changed.


## Entry checkpoint visibility test — 26 November 2026

Added entry_checkpoint.S and make_entry_checkpoint.py. The builder validates
Boot_Video offsets against the pinned header and compiles relocation-free ARM64
code. It modifies only the entry trampoline and verified zero/NOP padding in a
separate VMAPPLE copy, preserves the input, and changes the UUID. These images
intentionally halt after attempting a red 128x64 tile.

Both Apple RELEASE control variants stayed black. Version 2 accepts packed
30-bit and 32-bit pixels and cleans cache lines before a barrier. Marker
visibility is NOT validated: absence cannot establish whether entry executed.
The diagnostic was not applied to the source-built kernel.

A private Virtualization debugger helper was terminated by the host with exit
137 before help/main when signed with its private entitlement. No debugger was
attached or host security policy changed. Serial silence is inconclusive too:
the previously successful Apple control also produced no Tart serial output.

The exact source-built kernel failure remains unresolved. Evidence:
out/vmapple/checkpoints/boot-test-result.json and per-image build reports.
The working Apple control is being restored following these tests.

After the entry-marker tests, the working Apple control was restored and
booted successfully to its login screen. Host boot policy was unchanged.


## Standalone Apple kernel test — 26 November 2026

At the user request, installed the unmodified KDK kernel.release.vmapple
directly as a Mach-O custom boot object, without linking or supplying a driver
collection. SHA-256: bac2f7f640de0f41d3f5bde45e703957e1becbe2c182813c1c10588582a8a52d.
Kernel UUID: 8012A15E-57F1-32BD-903A-48F2332790BB.

The guarded Recovery installer accepted the Mach-O, compressed it, installed it
successfully and read back boot arguments. The normal VM boot remained black
at approximately 35 and 85 seconds, with no visible verbose output and no reply
to two ICMP probes. No running kernel identity or execution checkpoint was
obtained. This test does not establish which startup stage was reached. A full
macOS boot cannot be assumed without its platform and storage drivers.

This used Mach-O loading, not kmutil --raw, which is for a flat boot payload
with explicit mapping and entry addresses. No collection was rebuilt for this
test. Evidence: out/vmapple/stock-kernel-test.json. Restore of the previously
working Apple kernel plus release-driver collection follows.

The Apple control collection was then restored and booted successfully to
the login screen. The standalone test did not change host boot policy.


## Build-setting isolation and observable entry — 26 November 2026

Extended build_vm_baseline.py and link_vm_baseline.py with isolated no-LTO
and RELEASE configurations. Both the unpatched DEVELOPMENT/no-LTO and
RELEASE/no-LTO kernels built and linked with the exact working control driver
UUIDs (218 identifiers). Recovery installed each successfully; both stayed black
on repeated observations and did not answer two ICMP probes. Disabling LTO and
changing build configuration have not fixed boot. The DEVELOPMENT no-LTO
12-class vtable check passed; the RELEASE comparison encountered unresolved
targets in the installed Apple RELEASE reference, so it is inconclusive.
Build reports: out/vmapple/baseline-no-lto and baseline-no-lto-release.

Re-signed the debugger helper using only the public virtualization entitlement.
It launches, but VZ refuses VM startup with the private debug stub enabled.
The same cloned configuration starts without the stub. A private PL011 serial
configuration also validates but fails VM startup. Host policy was unchanged.

Added a poweroff mode to entry_checkpoint.S and make_entry_checkpoint.py.
It issues PSCI SYSTEM_OFF (w0=0x84000008, x1-x3=0, hvc #0), matching the sequence
in the KDK AppleVirtualPlatform ApplePSCI::_systemOff implementation.
Unlike the framebuffer marker, THIS ENTRY SIGNAL IS OBSERVABLE: the Apple
kernel control with entry replaced and release drivers linked stops in under
one second, exit 0, with Tart reporting guest has stopped the virtual machine.
A repeat boot reproduced the same result. Evidence:
out/vmapple/checkpoints/apple-poweroff.json. Source kernel checkpoint testing
is in progress. These intentionally stopping images are diagnostics, not boot
fixes or Linux ABI passes.

The source-built RELEASE/no-LTO kernel with the same entry shutdown probe
also stopped the VM successfully (Tart exit 0, guest has stopped the virtual
machine). Kernel entry execution is now verified for that source image.
Evidence: out/vmapple/checkpoints/source-poweroff.json. The next checkpoint
is arm_init to isolate assembly startup from C initialization.

The source arm_init function checkpoint also stopped the VM successfully
(Tart exit 0). Assembly startup and entry into C initialization are verified.
The next checkpoint is kernel_bootstrap, after early platform/memory setup.
Evidence: out/vmapple/checkpoints/source-arm-init.json.

Added make_collection_checkpoint.py to patch named functions AFTER linking,
so replacement instructions do not invalidate split-segment relocations.
The source PE_init_platform checkpoint successfully shut the guest down,
validating this method and execution through relocation setup. The source
kernel_bootstrap checkpoint did not fire (guest remained running). The next
probe is arm_vm_init. Evidence: checkpoints/source-pe-init.json and
source-bootstrap-linked.json under out/vmapple.

The arm_vm_init entry checkpoint and the subsequent serial_init entry checkpoint both successfully shut the VM down. Memory initialization returns; the black boot occurs later. serial_init evidence: out/vmapple/checkpoints/source-serial-init.json, Tart exit 0 and /tmp/rdxnu-vm-serial-init.log. Next checkpoint: PE_create_console after serial initialization and initial startup callbacks.

2026-11-26: Post-link shutdown checkpoints narrow the source RELEASE/no-LTO
black boot to the final early_random() call in arm_init (arm_init.c:685).
Positive checkpoints (Tart exit 0, guest has stopped): serial_init,
PE_create_console, cpu_machine_idle_init, pe_arm_debug_init_early,
pe_init_fiq, arm_init+0xaf4 (after PE_init_cpu), arm_init+0xb04
(immediately before early_random). arm_init+0xb08 (immediately after
that call) does not fire; neither does machine_startup. These probes use
the same base collection and all 218 release driver identities. The earlier
arm_vm_init call to early_random completes, so this is specifically the
later call/context, not proof that entropy initialization is generally broken.
No fix yet; PRNG state, callback authentication, and the changed mapping/CPU
context need investigation. Reports live in out/vmapple/checkpoints/source-*.json.
make_collection_checkpoint.py now accepts a bounds-checked, aligned --offset
within a function for precise call-site probes. All probes intentionally stop;
none is a working OS candidate. Restoring the Apple control next to inspect
any persisted panic diagnostics and provide a working VM display.

The Apple control has now been restored and booted to a visibly working macOS login screen in rdxnu-25f80. The VM is left running there. This restores the display using the control kernel; the source kernel remains unresolved. Recovery diagnostics collection found no recent kernel/panic report files.

2026-11-26: Found and corrected the late early_random blocker. Source-level,
boot-argument-selectable checkpoints (build_prng_diagnostic.py) establish:
late DRBG callback entry, digest loads, Vprev initialization/copy, HMAC return,
health check and output copy all complete. The final update() reaches its
cchmac_di_decl allocation but does not pass it. Diagnostic stage 0 reproduces
the black display, ruling out an instrumented build that accidentally boots.

The arm64e __chkstk_darwin in Xcode 26.6's libclang_rt.cc_kext.a starts with a
branch, with no BTI landing instruction. Apple's 25F80 release VMAPPLE helper
starts with bti c. The kernel calls this helper indirectly with BLRAA; after
BTI guarding becomes active this entry is invalid. Replacing the branch to
the immediately following probe body with bti c preserves the helper and all
stack-memory probes, without moving code or disabling BTI/stack checking.

Runtime verification on the clean RELEASE/no-LTO baseline, with no PRNG
instrumentation: the one-instruction correction makes both machine_startup
and kernel_bootstrap shutdown checkpoints fire (Tart exit 0), whereas these
checkpoints were unreachable before. This resolves the PRNG startup blocker.
The corrected full collection still displays black; a later startup failure
remains. Do not claim a complete source-kernel boot or general driver support.

make_direct_stack_probe.py --mode bti creates a separate corrected VM-only
collection with matching-symbol checks and a fresh outer UUID. Only one code
instruction and the outer UUID change; embedded kernel/driver identities stay
unchanged. link_vm_baseline.py --stack-probe-bti exposes the correction as an
explicit option, preserving the baseline output. Reports: out/vmapple/
stack-probe-bti*.json. Source checkpoints restore exact original source bytes.
No physical host boot configuration or security policy was changed.

The corrected console_init entry checkpoint was not reached (guest remained
running). Current remaining interval: kernel_bootstrap entry to console_init,
including version printf, scale_setup, vm_mem_bootstrap, ATM and OSLOG startup.
The evidence does not yet identify which of these fails. Apple control restore
is in progress to leave the VM usable.

Apple control restoration is complete. rdxnu-25f80 is running with a visibly
verified macOS login screen. Active control collection SHA256:
befff4ce9544c00cf19bb5bcfb1f6906189864daa64ae37efdb74cb42b2402aa.
The BTI-corrected source collection is preserved separately for the next trace.


2026-11-26: The clean RELEASE/no-LTO VM kernel boots to a verified desktop
with BTI code generation and enforcement disabled in an isolated diagnostic
build. Kernel UUID F8631252-1709-3E97-96E9-8219205249A1; collection UUID
CCF1948F-643C-6E76-24EB-0CEE0F02CF1E. Running userspace reports 25F80;
Recovery-mounted volume metadata separately reports 25G342. The latter does
not establish a running-system version mismatch. Raw and collection hashes
and runtime logs are in out/vmapple/baseline-no-lto-release-no-bti and results.

Further checkpoints reach vm_mem_bootstrap but not atm_init. KDK platform
support includes address-taken pmap entries lacking BTI landing instructions;
the compiler stack helper is therefore not the only compatibility issue.
BTI-disabled full boot supports this diagnosis; no individual pmap fault ESR
was captured. This is a VM diagnostic workaround, not a BTI-enabled fix.

The desktop is only a kernel/driver baseline, not project acceptance. Target:
Arch Linux ARM userspace and Darwin application support sharing custom XNU,
Apple drivers and our broker/display path, without Apple WindowServer.
The booted baseline has no SVC probe, ELF support or Linux userspace proof.
Next smallest checkpoint: boot-gated P0 Linux syscall entry alongside a
working Darwin syscall control, using the existing native carrier.


2026-11-26: KERNEL_LINUX_ABI_P0_PASS on the custom DEVELOPMENT/no-LTO/no-BTI
VM kernel. Live UUID 9332E683-A155-3607-8B07-DBFD7D95947F; outer collection
UUID E31A67D2-BAD3-422D-0FD6-B4DC491114B9. The native Mach-O carrier executes
real SVC instructions: expected PID 560, Darwin PID 560, Linux PID 560, unknown
Linux syscall -38. Source and BTI header were restored after building. Exact
release driver UUIDs match the working Apple control (218 collection entries).
The host stock-kernel negative control terminates the marked child with SIGSYS.

The VM-only persistent launchctl overrides for com.apple.WindowServer and
com.apple.loginwindow were accepted and are present in the boot log. The
process snapshot after startup contains neither executable, while PID 1 is
/sbin/launchd and com.apple.powerd is running. This proves absence at the
observation point, not that no WindowServer ever ran, or that stock physical
macOS supports this deployment. The displayed login-like image alone cannot
establish process state. This experiment does not replace PID 1 or control
all startup services. A custom init/startup path is a separate milestone.

The first runner used the wrong virtiofs mount tag, so no log reached the
share. Recovery unlocked the Data volume and recovered the successful local
log: out/vmapple/results/p0-first-boot-local.log. The runner now uses Tart's
com.apple.virtio-fs.automount tag and the results subdirectory.

This is the smallest kernel-dispatch checkpoint. No Linux ELF loading, Arch
userspace boot, physical hardware operation, or full power management behavior
is established. BTI remains disabled solely for this VM diagnostic.

The second normal boot exported results/p0-boot.log successfully after the
virtiofs fix. Both kernel and outer collection UUIDs match the linked image;
P0 passed again, PID 1 is launchd, powerd is running, and the display processes
are absent at the check. Machine-readable evidence: out/vmapple/
p0-runtime-result.json. The VM is left running with this test kernel.
A custom init selector has not been implemented in this chunk; it remains
separate from these experimental launchd overrides.


2026-11-26: LINUX_ELF_M1_PASS on the custom VM kernel. The same static
AArch64 ET_EXEC bytes run on Linux 6.8.0-117-generic (AArch64 Docker host,
Arch Linux ARM userspace image) and XNU. ELF SHA256:
1d2d6f74d83f016dcd7d40401ad0124ffc3cb70f3344c4d3afdabe154b7bf11c.
Live kernel UUID A199E7E9-E994-38EF-85BF-628B355E279F; collection UUID
8E326F74-8A6F-6B0C-34C8-97CB6F9FDD14. Child PID 582 is returned by the
Linux getpid; write emits the expected 16 bytes and exit_group produces
status 37. Initial stack/auxv, BSS, ENOSYS, EFAULT and EBADF checks pass.
The Darwin parent and earlier P0 control pass on the same boot. The startup
process snapshot has no WindowServer/loginwindow; normal launchd remains.

Implementation: elf/elf_plan.h validates the bounded static format;
elf/linux_exec.h maps segments, builds a minimal Linux initial stack, and
dispatches getpid/write/exit/exit_group by a reserved process personality bit.
The common XNU exec path retains process/credential/descriptor cleanup.
elf_prepare.py temporarily patches the exact pinned sources and adds fork
inheritance plus personality reset on successful Darwin exec. All temporary
sources restore after building. No Linux instructions are rewritten and no
userspace syscall interceptor runs in this path. Host validator tests include
malformed headers and 20,000 deterministic mutations under ASan/UBSan.

This is a DEVELOPMENT VM diagnostic: BTI is disabled and unsigned ELF map
acceptance requires rdxnu_linux_elf=1. It is not a production signing policy.
Only bounded ET_EXEC with 16KB-compatible segment geometry is accepted;
PT_INTERP, dynamic loading, TLS and executable stacks are rejected. Initial
auxv and errno handling cover the first test subset, not glibc compatibility.
There is no Linux clone/execve/signal ABI implementation yet. Fork inheritance
and Darwin re-exec reset are implemented but not runtime-exercised from a
Linux process in this milestone. Physical operation and Arch boot remain false.
Evidence: out/vmapple/elf-runtime-result.json and results/elf-boot.log.


2026-11-26: first unmodified Arch component runs on XNU. Arch Linux ARM's
glibc 2.43+r22+g8362e8ce10b2-2 ld-linux-aarch64.so.1 --help prints the real
help text and exits 0. Runtime SHA256 matches the reference package file:
b1f2738bdb51e6419d7062f96c8a44bd68853698dc5ac624fa6ca9f9b03d456d.
The help text matches reference Linux after normalizing argv[0]. Kernel UUID
D37E470A-3C3B-303A-A319-F80DBCDFACC0; collection UUID
0C2796FC-A543-3145-25E1-48E6FF78148A. P0 and M1 regression controls pass
on the same boot. Evidence: out/vmapple/arch-loader-runtime-result.json
and results/loader-boot.log.

The separate --with-arch-loader candidate adds ET_DYN fixed load bias, expanded
auxv and writev/identity syscalls. PT_INTERP, TLS, heap and shared-library
loading remain unsupported. In particular, successful --help does not establish
correct brk behavior; its reference trace includes a brk query, while this
prototype still returns -ENOSYS for brk. No /usr/bin/true or glibc application
success is claimed. The Linux trace of true identifies next interfaces:
48,56,57,63,80,94,96,99,214,215,222,226,261,278,293.

The VM display retains its preboot image; no live Linux console or rendering
backend is attached. This image does not represent current guest process state.
After loader completion the guest replied to both fresh ICMP probes with no
packet loss. It is left running headlessly with the loader candidate.


## 26 November 2026: direct Arch executable paths on XNU

Kernel 10A390BE-A623-3DF9-8337-20BD4F3C71DD passes the new opt-in
`--with-linux-interp` checkpoint. XNU performs caller-root vnode lookup and
native exec/MAC permission checks for PT_INTERP before task replacement, then
maps program and interpreter into the native task. The unchanged Arch loader
initializes TLS and dependencies. The dynamic Linux probe verifies auxv,
actual exec filename independent of argv[0], environment, TLS, native chroot
path scope, and missing/denied/short interpreter errors preserving its state.
It forks and directly execs unchanged Arch printf, then reaps it. Unchanged
Arch systemd --version also executes directly in the root. Both stdout and
stderr match the immutable Linux reference, including the expected missing
proc warning. Existing outside-root version output still has empty stderr.
All earlier gates, Darwin parent/wait control and absent-WindowServer check
pass on the same boot, with visible Apple-driver console output.

Raw kernel SHA-256: 0fb27056d91f365d626ccd8008f141d64ac3ba27ecae899392011538c73fd1b6.
Collection SHA-256: 98d4c6b6cb6783ad6411cf165ff162750d8ec1602147a9d0b1164462fb3fb15b.
Staged identity: interp-10a390be-27516bf8.
See [interp/evidence.json](interp/evidence.json) for exact fixture/package
hashes, frozen proof-log hashes and unchanged native assembly offsets.

This removes the explicit-loader prerequisite for bounded dynamic service
executables. It does not establish a systemd manager, PID/init namespace,
procfs, complete filesystem sandbox, ASLR/text locking, interactive shell or
physical hardware support. The manager negative control still exits one
at its ordinary unbooted-context guard. The coherent Linux init/runtime
context and manager event interfaces remain the next systemd boundaries.


## 26 November 2026: native Linux event counter and poll readiness

Kernel D8C3FEDC-45FB-3AD3-9380-276D9B151910 passes the new opt-in
`--with-linux-events` profile. Linux eventfd2 creates a real XNU fileglob
backed by a mutex-protected counter, native read/write sleeps and native
kqueue IN/OUT filters. Native descriptor references preserve shared state
through dup, fork and an actual ELF exec, with close-on-exec respected.
Ppoll uses native poll's snapshot, kqueue registration and blocking scan,
with bounded flags/array/timeout conversion and truthful unsupported errors.
No userspace rendezvous socket, readiness-rescan worker or second fd registry
is introduced. Existing file/filter type values and native assembly offsets
are preserved; only private enum values are appended.

The unchanged Linux fixture passes counter/semaphore, saturation/EAGAIN,
copy-fault and transfer-size controls, descriptor lifetime, readiness, invalid
fd/timespec/pointer cases and timeout. A native supervisor holds back the
peer until it proves each of three separate waits is blocked: poll, empty
read and saturated write. All three release and the fixture exits zero.
A Darwin native pipe/kqueue control and all earlier checkpoints pass on
the same boot. The outside-root systemd version gate still has empty stderr;
direct exec/chroot stdout and stderr still exactly match Linux. WindowServer
is absent at the check and the event proof is visible on the Apple-driver
console. The manager diagnostic remains at the unbooted-context guard.

Raw kernel SHA-256: c03fe4475b7fb89f84f9eb90b8a701474b630855c8ac6a1f4dddf4ff4a2c93ef.
Collection SHA-256: 1d8700fb080c31d77f7eac13fe7ea1509643ebc7763f6b89fd1bfd80f05ed6af.
Staged identity: events-d8c3fedc-76505887.
See [events/evidence.json](events/evidence.json) for source identities,
fixture hash, reference kernel/image and frozen proof-log hashes.

Limits remain explicit: no atomic ppoll signal mask, at most 128 entries,
IN/OUT readiness and millisecond native timeout granularity. Anonymous
fstat metadata, F_SETFL, full vector-I/O/ioctl behavior and concurrent-thread
close races are not qualified. The Linux reference is kernel 6.8.0-117-generic
with the independently pinned Arch image; the image alone does not pin Linux.
Epoll, timerfd, signalfd, proc/cgroups and coherent init/runtime context remain
next systemd work. No manager, interactive shell or physical-driver pass is
claimed. rdmetal and scanout are unchanged.
