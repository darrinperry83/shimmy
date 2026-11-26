# Target: preserve the complete stock hardware stack

The acceptance target is the complete matched Apple hardware stack for this Mac,
not a reduced set chosen because individual drivers happen to link. Diagnostic
profiles remain useful for locating failures; passing them does not satisfy the
whole-machine requirement.

Reference captured on 25 November 2026:

- Mac14,5 / j414cap, T6020, macOS build 25F80.
- 256 loaded Apple components, including kernel KPI components.
- 357 entries in the actual active stock boot kernel collection, including the
  kernel. All loaded non-KPI driver IDs occur there and their UUIDs match.
- 15 board-specific firmware-update-directory IM4P payloads whose SHA-384 digests
  match the board/update identity in the matching restore BuildManifest.
- 3,549 installed on-demand firmware resource files, hashed and inventoried.
- 22 installed DriverKit extensions, inventoried separately from kernel drivers.

These are observations of the running stock system and its files. They do not
establish that our custom kernel loads the same stack.

## Preserve the boot and runtime layers

1. Retain Apple's device-appropriate boot/firmware environment. The chosen
   boot-objects path and boot-manifest hash, read from IODeviceTree, identify the
   actual reference. Do not use a different model's restore identity or assume
   the generic `/System/Library/KernelCollections` file is the ARM boot image.
2. Supply the complete stock collection driver set and any additional stock
   components observed loaded. Do not filter out SEP, audio, camera, GPU or
   other drivers to manufacture a successful link.
3. Preserve board-specific firmware, shared SEP/system firmware, and the full
   on-demand firmware resource trees. Checking FUD images alone is insufficient.
4. Keep the required Darwin service and DriverKit environment initially. A
   Linux desktop does not replace the process runtime that Apple driver
   extensions and hardware services expect. Remove services only after their
   dependencies and replacement behavior are demonstrated.
5. Add Linux process support to that hardware-capable base. Keep desktop and
   WindowServer changes separate until the stock hardware baseline survives.

Actual hardware matching still determines which installed drivers initialize.
The installed catalog contains support for other configurations; forcing every
such driver to attach to this one machine is not the acceptance criterion.

## Current whole-stack findings

The pre-boot command now captures the stock inventory and extracts the active
stock IMG4/LZFSE kernel collection locally, then checks both `stock_loaded` and
`stock_boot` driver sets. Extraction reads the reference and writes only `out/`;
it does not modify any boot object. Image4 signature verification is not claimed
by the extraction tool. Firmware digest comparison uses the exact embedded IM4P
object, not the personalized outer IMG4 wrapper.

The rebuilt kernel now supplies all **16 imports required by SEPHibernation**
through reconstructed SEPHibernator and SEPHibernatorService base interfaces.
Object sizes, field offsets, vtables, pointer authentication and selected base
behavior match the KDK evidence. See SEP-HIBERNATION.md. Apple's concrete driver
retains the SEP operations; no authentication or hibernation result is fabricated.
Kernel-wide hibernation remains disabled in the public configuration, so full
hibernate/restore support is not established.

Four battery/RAID imports remain unresolved against both raw kernel/dependency
controls. The complete native collection link accepts the set. No symbol
allowlist or driver exclusions are used. The output has exactly the stock
collection's 357 entry IDs and the custom kernel UUID, and passes kmutil inspect.

The earlier four GPU imports were a checker error, not a kernel deficiency:
IOGraphicsFamily is a flat-layout bundle and provides IOAccelerator on arm64e.
The executable lookup is corrected and covered by a regression check. GPU,
display, media, camera and all three audio diagnostic profiles now have no
unresolved imports or inspection errors.

## Acceptance gates

- Complete inventory: loaded driver IDs, stock boot collection, on-demand
  assets, firmware and DriverKit catalog, with explicit unknowns.
- Static compatibility: no unresolved required imports, actual binding and
  dependency/version checks, matching shared ABI surfaces.
- Offline custom collection: Apple's linker accepts the complete selected set;
  its contents and kernel identity are inspected afterward.
- Firmware availability: correct board/build inputs, loader handoff, shared SEP
  requirements and runtime firmware lookup paths preserved.
- Hardware test: driver initialization, storage, display/GPU, input, audio,
  network, camera, sleep/wake and other peripherals checked against stock.

No custom-kernel hardware test has been performed. Full loading, device-specific
runtime firmware mapping, DriverKit/service startup, and runtime functionality
remain unproven. The offline pre-boot gate passes. Boot encoding, personalization,
loader handoff and an alternate boot target remain to be established.

Reports are local and ignored under `out/driver-check/`: `stock-inventory.json`,
`stock-collection.json`, `symbol-report.json`, `vtable-report.json`, and
`preboot-report.json`, `sep-interface-audit.json`, and `full-stock-link-result.json`.
