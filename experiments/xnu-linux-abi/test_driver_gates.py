#!/usr/bin/env python3
"""Negative controls using the real local driver inventory and kernel images."""
import copy
import json
from pathlib import Path
from driver_gate import OUT, inventory, closure, PROFILES, symbols, unresolved_imports
from vtable_gate import Image, compare, CLASSES

dependency = json.loads((OUT / "prepared-dependencies.json").read_text())
kdk = Path(dependency["kdk_root"])
bundles = inventory(kdk / "System/Library/Extensions")
assert bundles["com.apple.driver.AppleSMC"]["dependency_key"] == "OSBundleLibraries_arm64"
assert "com.apple.driver.AppleARMPMU" in bundles["com.apple.driver.AppleSMC"]["dependencies"]
assert "com.apple.kernel" in bundles  # flat System.kext/Info.plist
graphics = Path(bundles["com.apple.iokit.IOGraphicsFamily"]["executable"])
assert graphics.is_file() and graphics.parent.suffix == ".kext"
assert {"__ZTV13IOAccelerator", "__ZN13IOAccelerator10gMetaClassE",
        "__ZN13IOAcceleratorC2EPK11OSMetaClass", "__ZN13IOAcceleratorD2Ev"} <= symbols(graphics, True)
ids, missing, _ = closure(PROFILES["storage"], bundles)
assert not missing and "com.apple.driver.AppleARMPMU" in ids
broken = dict(bundles)
del broken["com.apple.driver.AppleARMPMU"]
assert "com.apple.driver.AppleARMPMU" in closure(PROFILES["storage"], broken)[1]

custom_path = OUT / "xnu/BUILD/obj/DEVELOPMENT_ARM64_T6020/kernel.development.t6020.unstripped"
custom = Image(custom_path)
stock = Image(kdk / "System/Library/Kernels/kernel.development.t6020")
actual = custom.table(CLASSES["IOService"])
reference = stock.table(CLASSES["IOService"])
assert not compare(actual, reference)
swapped = copy.deepcopy(actual)
assert not set(swapped[0]["symbols"]) & set(swapped[1]["symbols"])
swapped[0], swapped[1] = swapped[1], swapped[0]
assert len(compare(swapped, reference)) == 2
auth = copy.deepcopy(actual)
auth[0]["authentication"]["diversity"] ^= 1
assert len(compare(auth, reference)) == 1
assert compare(actual[:-1], reference)

exports = symbols(custom_path, True)
driver = Path(bundles["com.apple.driver.AppleHIDKeyboard"]["executable"])
imports = symbols(driver, False)
ids, missing, _ = closure(["com.apple.driver.AppleHIDKeyboard"], bundles)
providers = set()
for identifier in ids:
    executable = bundles[identifier]["executable"]
    if executable:
        providers |= symbols(Path(executable), True)
candidate = next(s for s in (imports & exports) - providers if s.startswith("_PE_"))
assert not unresolved_imports(imports, providers, exports)
assert unresolved_imports(imports, providers, exports - {candidate}) == [candidate]
print("DRIVER_GATE_NEGATIVE_CONTROLS_PASS: missing dependency, missing kernel export, swapped slot, PAC mismatch, truncated table")
