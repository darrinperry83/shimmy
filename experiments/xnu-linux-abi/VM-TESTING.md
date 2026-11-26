# VM test route

A macOS guest can test the modified XNU syscall entry without rebooting the
host. It requires a VMAPPLE kernel and the guest's matching driver collection.
The physical T6020 collection is retained separately; it is not a VM boot image.

## Test scope

The runtime criterion is the native P0 probe executing under our audited
DEVELOPMENT_ARM64_VMAPPLE kernel with rdxnu_linux_probe=1. Linux getpid must equal
Darwin getpid, an unknown Linux syscall must return -38, and Darwin calls must
continue to work. vm_probe.sh additionally requires virtualization, build 25F80
and the expected running kernel UUID. A stock guest boot is only a control.

Virtual devices do not exercise the Mac's physical GPU, SEP, fingerprint reader,
audio controllers, or other hardware. A successful VM probe would not establish
physical driver compatibility or complete Linux userspace support.

## Local preparation

Build the VM variant from the same pinned source and patch:

```sh
python3 experiments/xnu-linux-abi/build_kernel.py --machine VMAPPLE --build
python3 experiments/xnu-linux-abi/vtable_gate.py --machine VMAPPLE
```

Logs, firehose library and audit reports are under out/vmapple. The kernel is
out/xnu/BUILD/obj/DEVELOPMENT_ARM64_VMAPPLE/kernel.development.vmapple.
The default T6020 artifacts and reports remain in their existing locations.
Both builds share a lock to avoid simultaneous source/build preparation.

Tart 2.42.0 is extracted under out/vm-tools, with its archive SHA-256 checked
against the release asset digest and its application code signature verified.
Archive SHA-256:
a95dda214b5f676298430d665a8174992e6b300dd20c653385b60481ae819ddf.
Tart's application executable is used directly, without a systemwide install.
TART_HOME points to this experiment's out/vm directory.

The VMAPPLE build and linked syscall audit pass. Its UUID is
B4BDC5FA-9643-3732-B891-802487AFE5DB; SHA-256 is
3d7e75b88cee87e97edd7e7cd754892dc59d1138bd4001e81426be946aaab0a8.
The 12 core IOKit vtables match the Apple VMAPPLE control (1,297 slots).
These results are recorded under out/vmapple. They are not a boot result.

A disposable rdxnu-25f80 guest has been installed from Apple's exact
UniversalMac_26.5.1_25F80_Restore.ipsw. No existing VM was found or modified.
The download is checked against the SHA-256 supplied by Apple's CDN:
83f53dc7f9cce465ba88dd487bda74ce60c5582fcd45251eeeb9aea7f5a72f4d.

The restore source is:
https://updates.cdn-apple.com/2026SpringFCS/fullrestores/122-88870/E47EBB85-45F2-4E3C-B9E7-6FF7868C2FBA/UniversalMac_26.5.1_25F80_Restore.ipsw

## Guest boot preparation

After installation, establish the stock guest baseline and capture its collection
and driver IDs. link_vm_collection.py accepts an encoded or raw guest collection,
requires its kernel UUID to match Apple's 25F80 RELEASE VMAPPLE control, and links
the complete guest entry set. It verifies exact output IDs, custom kernel UUID
and kmutil inspection. For example, with a copied guest collection:

```sh
python3 experiments/xnu-linux-abi/link_vm_collection.py /path/to/guest-kernelcache
```

The output is out/vmapple/vm-boot.kc, with the command, inspection and result beside
it. The helper rejects the physical collection as a negative control. Keep the physical machine's 357-entry collection out of this step.

Custom-kernel boot configuration belongs exclusively to this disposable guest.
It requires guest recoveryOS and guest policy allowing custom boot objects.
The upstream build project documents this flow:
https://github.com/blacktop/darwin-xnu-build#boot-the-kernel-collection-in-a-vm
The host's security settings and boot objects must remain unchanged.

After the experimental guest boot, run inside the guest:

```sh
sh vm_probe.sh EXPECTED_VM_KERNEL_UUID ./probe
```

Record the full output and running kernel identity. Only VM_KERNEL_PROBE_PASS
following the real probe establishes this runtime gate. Neither compilation,
collection linkage, guest installation nor an ordinary stock boot counts.

## Restore collection result

The VM restore manifest identifies vma2macosap and kernelcache.release.vma2.
The retrieved member matches its SHA-384 IM4P digest, and the embedded kernel
UUID matches the KDK's RELEASE VMAPPLE kernel. Apple's linker accepts all 218
entries with the custom kernel. The output passes exact entry-set and kernel
UUID checks and kmutil inspect. These are offline checks; the guest has not
booted the custom kernel yet. The full restore archive SHA-256 now matches Apple's published CDN digest;
the extracted VM member matches the completed archive too.

The transfer.dmg under out/vmapple carries the collection, native probe,
VM-only wrapper and expected UUID for use only in the disposable guest.

## Runtime stage — 26 November 2026

The disposable guest rdxnu-25f80 has 4 CPUs, 8 GB RAM and a 64 GB disk.
First-boot setup is complete. Its stock runtime baseline reports 25F80,
Darwin 25.5.0, RELEASE_ARM64_VMAPPLE, and kernel UUID
8012A15E-57F1-32BD-903A-48F2332790BB. All 118 loaded components match the
restore collection IDs/UUIDs. The transferred probe hash matches the host
artifact; its marked syscall child dies with SIGSYS, producing
STOCK_NEGATIVE_CONTROL_PASS and GUEST_STOCK_BASELINE_PASS.
Reports: out/vmapple/stock-runtime.json and results/stock-baseline.log.

The guest was shut down cleanly, then cloned through Tart as
rdxnu-25f80-stock before boot-policy changes. Recovery authentication completed,
and the user authorized unrestricted changes to this disposable guest. SIP is
now disabled in the working guest. bputil reports Permissive Security, disabled
kernel CTRR and boot-argument filtering. The host's boot/security settings are
unchanged.

Recovery has neither shasum nor openssl. file_sha256.c supplies a native
CommonCrypto file hasher; empty-vector and actual-collection comparisons pass.
The guarded guest_recovery.sh uses that utility and passed its VM, guest build
and collection hash checks in recoveryOS. The user corrected the supplied guest
credential, and the native PTY helper completed owner authentication. kmutil
reported installing the boot object and done. bputil then showed a custom-KC
hash (coih), confirming the installed policy.

The unqualified nvram boot-args write was refused with not permitted, even
after restarting recoveryOS. Addressing the variable explicitly through Apple's
system-variable GUID (40a0ddd2-77f8-4392-b4a3-1e7304206516) succeeded and read back
-v rdxnu_linux_probe=1 -unsafe_kernel_text keepsyms=1 debug=0x100.
guest_recovery.sh now uses and reads back that qualified variable.

Both system-disk boot attempts remained black, including the retry after the
qualified boot-argument update. Tart's attached serial console captured zero
bytes; no running custom kernel UUID or probe result has been obtained. This
is an unresolved early startup failure, not a runtime pass. The working VM was
restarted into recovery options; the stock checkpoint remains intact.
out/vmapple/recovery-state.json records the observed installation state.
Temporary host transfer servers are stopped. Only the transfer disk and the
dedicated out/vmapple/results folder are shared with the guest.

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

2026-11-26 checkpoint update: source RELEASE/no-LTO boot is proven through
arm_init+0xb04 (0xfffffe0007a36820), immediately before its final early_random()
call. The adjacent marker at +0xb08 does not fire. Memory initialization,
serial, console, CPU idle, interrupt setup, and timer enablement return.
This is a localization, not a fixed kernel. The earlier early_random() call
in arm_vm_init does complete. The exact PRNG failure path remains unknown.
Checkpoint metadata: out/vmapple/checkpoints/source-after-timer.json and
source-after-random.json. The diagnostic tool supports aligned --offset
within a function; post-link patches preserve relocation processing.
Recovery installation of the working Apple control completed successfully;
current verified runtime state is tracked in out/vmapple/current-boot-state.json.
Recovery search found no recent persisted kernel/panic reports.

### Verified stack-probe BTI correction (2026-11-26)

The clean source RELEASE/no-LTO kernel now reaches machine_startup and
kernel_bootstrap with a one-instruction BTI correction to __chkstk_darwin.
Full normal boot remains unresolved. This is separate from the SVC probe.

To link a separately named corrected candidate after a completed baseline build:

```sh
python3 experiments/xnu-linux-abi/link_vm_baseline.py --no-lto --configuration RELEASE --stack-probe-bti
```

The helper's original branch to the next instruction is replaced by BTI C;
fall-through executes the unchanged stack probe. This does not disable BTI or
stack checks. The exact input must match the unstripped kernel UUID. Review
both collection-result.json and the corrected candidate's adjacent JSON.
The intentionally stopping startup/bootstrap images are diagnostic only.


### Isolated BTI diagnostic and P0 entry

The VM-only --no-bti option disables both compiler BTI generation and
BTI_ENFORCED mapping policy. BTI_BUILD=0 alone does not disable enforcement.
The builder restores the exact source/header bytes after building. Default
builds retain BTI. The no-BTI variant is a diagnostic, not a secure release.

```sh
python3 experiments/xnu-linux-abi/build_vm_baseline.py --no-lto --no-bti --configuration RELEASE
python3 experiments/xnu-linux-abi/link_vm_baseline.py --no-lto --no-bti --configuration RELEASE
```

To retain the existing P0 SVC dispatcher, use a separately named DEVELOPMENT
variant (RELEASE deliberately rejects this option):

```sh
python3 experiments/xnu-linux-abi/build_vm_baseline.py --no-lto --no-bti --with-svc-probe
python3 experiments/xnu-linux-abi/link_vm_baseline.py --no-lto --no-bti --with-svc-probe
python3 experiments/xnu-linux-abi/stage_vm_p0.py
```

This uses the working control's release driver identities with a development
kernel. Building/linking never installs or boots. A live UUID check and the
carrier's KERNEL_LINUX_ABI_P0_PASS are required before claiming kernel entry.
The marked Mach-O carrier is not an ELF execution or Arch userspace test.


### Isolated P0 kernel and headless launchd profile

Build and link the DEVELOPMENT probe with the VM-only BTI workaround:

```sh
python3 experiments/xnu-linux-abi/build_vm_baseline.py --no-lto --no-bti --with-svc-probe
python3 experiments/xnu-linux-abi/link_vm_baseline.py --no-lto --no-bti --with-svc-probe
python3 experiments/xnu-linux-abi/stage_vm_p0.py
```

This writes baseline-no-lto-no-bti-svc-probe, preserving the working RELEASE
control. The linker retains the control's exact release driver UUIDs. The
builder retains the pinned SVC patch, disables BTI code generation and mapping
enforcement temporarily, and restores source files after building. Probe builds
require DEVELOPMENT; RELEASE continues to exclude the dispatch. BTI disabling
is diagnostic only, not a production security configuration.

guest_p0_setup.sh runs only as root inside the 25F80 VMAPPLE guest. Its staging
directory must contain p0-probe, guest-p0-boot.sh, and p0-job.plist. The job's
ProgramArguments must pass the expected kernel UUID to the runner. Setup saves
the previous disabled-service report under /Library/RDXNU-P0 and installs
org.rdxnu.p0 as a RunAtLoad LaunchDaemon. It requests persistent disable overrides for WindowServer and loginwindow
for the next boot without killing the current desktop or replacing launchd.
It does not edit the sealed system volume.

The runner records /var/tmp/rdxnu-p0.log and exports it as results/p0-boot.log
through mount_virtiofs. It checks the kernel UUID, executes the native syscall
probe, waits for system startup, checks process absence, and records powerd
status. A black screen alone is neither success nor failure for this profile.
The marked Mach-O carrier is a kernel-entry proof, not Linux ELF or Arch boot.

To restore the UI from a guest root shell, enable system/com.apple.WindowServer
and system/com.apple.loginwindow with launchctl, boot out system/org.rdxnu.p0
if loaded, and remove the test LaunchDaemon before rebooting. If the guest is
headless and remote access is unavailable, use Recovery and restore the two
service overrides in the guest data volume's launchd disabled.plist, preserving
all unrelated entries. Inspect disabled-before.txt first.


### M1 ELF checkpoint

The isolated --with-elf variant now executes a static Linux ELF and preserves
the native Darwin parent. See [elf/README.md](elf/README.md) for build, staging,
reference-Linux checks, guest execution and scope limits. Runtime identities
and results are recorded in out/vmapple/elf-runtime-result.json.
