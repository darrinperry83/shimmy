# Apple driver baseline before boot

Requirement: do not attempt a hardware boot merely because XNU compiles. First
establish a candidate Apple driver set, resolve its binary interfaces, compare
shared IOKit ABI surfaces, and link an offline kernel collection. Runtime device
support remains a later hardware test; none of the checks here claims it.

The whole-machine acceptance target is described in STOCK-HARDWARE-TARGET.md.
The profiles below are diagnostic subsets; the full stock collection and loaded
driver sets are also mandatory checks.

The inputs are the built DEVELOPMENT T6020 kernel and the Apple-signed 25F80 KDK
already downloaded and verified by `build_kernel.py`. The matching Apple
DEVELOPMENT T6020 kernel is the comparison control.

## Candidate sets

| Set | Root drivers | Bundles with dependencies | Static imports |
| --- | --- | ---: | --- |
| Platform | AppleARMPlatform, AppleT6020, ApplePMGR, AppleT6021PMGR, AppleInterruptControllerV2 | 10 | All resolved |
| Input | AppleHIDKeyboard, AppleHIDKeyboardEmbedded, AppleMultitouchDriver, AppleSPU, AppleHIDTransportFIFO, AppleActuatorDriver, AppleTopCaseHIDEventDriver | 34 | All resolved |
| Storage | IONVMeFamily, APFS | 32 | All resolved |
| GPU | AGXG14X, IOGPUFamily, IOSurface, IOGraphicsFamily, G14X firmware-interface kexts, DART | 23 | All resolved |
| Display | AppleDCP, AppleMobileDispT602X-DCP, IOMobileGraphics, display crossbar, DisplayPort proxies/TCON, HDCP | 52 | All resolved |
| Video codecs/scaling | AppleAVD, AppleAVE2, AppleProResHW, AppleJPEGDriver, AppleM2ScalerCSCDriver | 13 | All resolved |
| Camera | AppleH13CameraInterface | 7 | All resolved |
| Built-in audio | T602x MCA/DMA, embedded/IIS/CS42L84 audio, isolated-stream/Exclaves interfaces | 23 | All resolved |
| AOP audio | AppleAOPAudio, AppleAOPVoiceTrigger | 25 | All resolved |
| USB audio | AppleUSBAudio and its dependencies | 33 | All resolved |
| Full loaded stock set | 256 observed Apple components; KPI entries supplied by kernel | 250 | 1 unresolved in both raw controls; full native link accepted |
| Full stock boot set | All driver entries from the active stock collection, plus dependencies | 358 | 4 unresolved in both raw controls; full native link accepted |

The diagnostic sets overlap. The full-stock sets supersede their reduced scope
for acceptance. Input transports and platform
roots were selected with reference to `kmutil showloaded` on this Mac, rather
than assuming a keyboard class driver alone is enough. This is not an exhaustive
I/ORegistry provider/matching graph. The video roots likewise reflect this Mac's
loaded G14X GPU, T602X display, codecs and H13 camera interfaces. Firmware payload
packaging, complete USB/network behavior and Touch ID are not established by
symbol checks. Runtime video and audio operation are also not proven.

`driver_gate.py` follows ARM64-specific OSBundleLibraries overrides and includes
nested/flat kext metadata. It resolves executable imports against the kernel and
the selected dependency closure, reports missing names, and repeats the check
against Apple's kernel. KPI pseudo-bundles are supplied by the kernel. Driver
binary hashes and dependency versions are captured in the report. Version
compatibility and actual binding still need the collection linker; symbol-name
resolution alone does not verify those or object layouts.

## Video findings

Display includes both panel-side DCP/mobile-display drivers and the loaded
DisplayPort/crossbar/HDCP stack. GPU coverage includes IOSurface/IOGPU, G14X AGX,
DART, and the loaded firmware-interface kexts. Including a firmware-interface
kext is not proof that the required firmware payload has been packaged.

The earlier four unresolved GPU imports were a checker bug: the executable in
the flat-layout IOGraphicsFamily bundle was not inspected. It supplies the
IOAccelerator symbols on arm64e. That lookup is corrected and regression-tested;
the GPU chain now passes the static import check.

Kernel video drivers alone do not supply Metal, VideoToolbox or camera APIs.
The intended system retains the matching Apple userspace services/frameworks;
rdmetal and the existing presentation code will still need runtime verification
against that stack. This work does not change those rendering paths.

## Shared IOKit ABI check

| Class | Method slots compared | Slot/authentication mismatches |
| --- | ---: | ---: |
| OSObject | 39 | 0 |
| IOService | 272 | 0 |
| IOUserClient | 306 | 0 |
| IOMemoryDescriptor | 77 | 0 |
| IOWorkLoop | 67 | 0 |
| IOCommandGate | 75 | 0 |
| IOMemoryMap | 56 | 0 |
| IODMACommand | 71 | 0 |
| IOBufferMemoryDescriptor | 100 | 0 |
| IOGeneralMemoryDescriptor | 77 | 0 |
| IOInterruptEventSource | 74 | 0 |
| IOTimerEventSource | 83 | 0 |
| SEPHibernator | 48 | 0 |
| SEPHibernatorService | 272 | 0 |
| SEPHibernator::MetaClass | 30 | 0 |
| SEPHibernatorService::MetaClass | 30 | 0 |

`vtable_gate.py` reads the real Mach-O vtables in both kernels. It resolves
authenticated kernel-relative pointers using the SDK's arm64e authenticated
rebase representation and compares exact target-symbol aliases, ordering,
pointer-authentication diversity, address-diversity and key. It rejects unknown
pointer encodings. This covers 16 vtables (1,677 actual method slots), excluding trailing alignment
padding. It does not cover every class, instance field, calling convention or
private interface used by the drivers. SEP-HIBERNATION.md describes the additional
SEP object-layout and generated-behavior checks.

`test_driver_gates.py` checks failure detection by removing an actual dependency
and an imported kernel export, swapping real vtable slots, changing a PAC
diversity value, and truncating a table. These controls pass.

## Pre-boot gate

From the repository root:

```sh
python3 experiments/xnu-linux-abi/preboot_driver_gate.py
```

The command runs the stock firmware/collection inventory, kernel artifact audit,
all driver symbol profiles, vtable and SEP interface comparisons, negative
controls, and a complete offline kmutil link and contents inspection. Output
paths are under out/driver-check; it does not install a collection, load a driver,
alter security policy, or boot the machine. Any failed check returns nonzero.

Reports:

- `out/driver-check/symbol-report.json`
- `out/driver-check/vtable-report.json`
- `out/driver-check/sep-interface-audit.json`
- `out/driver-check/preboot-report.json`
- `out/driver-check/full-stock-link.log`
- `out/driver-check/full-stock-link-result.json`

Current result: **DRIVER_PREBOOT_GATE_PASS**. All diagnostic profiles, ABI,
artifact, firmware-digest, stock inventory and negative-control checks pass.
There are no additional unresolved imports versus Apple's raw control, missing
dependencies or binary inspection errors. The four common raw boot-set imports
remain reported; acceptance requires the complete native collection link, not
an ignored-symbol list.

The matching 25F80 KDK is installed in /Library/Developer/KDKs and both package
receipts are present. Apple's linker accepts the custom kernel with every stock
boot driver and any additional loaded driver. No driver is elided. The resulting
full-stock.kc has exactly the stock collection's 357 entry IDs, no missing or
extra entries, and the custom kernel UUID. It also passes kmutil inspect.

stage_boot_candidate.py copies the linked collection and unchanged reference
firmware into a local, hash-verified preflight bundle. This does not encode or
personalize the collection for a new boot identity or install it. Runtime
firmware and DriverKit still depend on the matching stock root.

Do not boot until the offline collection gate passes and the alternate boot
target and matching firmware/kernel-collection plan are established. A passing
gate establishes static readiness only. Hardware initialization, filesystem
mounting, input, and biometrics still require runtime tests.
