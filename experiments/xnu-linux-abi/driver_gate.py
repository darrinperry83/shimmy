#!/usr/bin/env python3
"""Check candidate Apple drivers offline; never loads drivers or alters boot."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = Path(__file__).resolve().parent
OUT = HERE / "out"
NM = subprocess.check_output(["xcrun", "--find", "llvm-nm"], text=True).strip()
PROFILES = {
    "platform": ["com.apple.driver.AppleARMPlatform", "com.apple.driver.AppleT6020",
                 "com.apple.driver.ApplePMGR", "com.apple.driver.AppleT6021PMGR",
                 "com.apple.driver.AppleInterruptControllerV2"],
    "input": ["com.apple.driver.AppleHIDKeyboard", "com.apple.driver.AppleHIDKeyboardEmbedded",
              "com.apple.driver.AppleMultitouchDriver", "com.apple.driver.AppleSPU",
              "com.apple.driver.AppleHIDTransportFIFO", "com.apple.driver.AppleActuatorDriver",
              "com.apple.driver.AppleTopCaseHIDEventDriver"],
    "storage": ["com.apple.iokit.IONVMeFamily", "com.apple.filesystems.apfs"],
    "video_gpu": ["com.apple.AGXG14X", "com.apple.iokit.IOGPUFamily",
                  "com.apple.iokit.IOSurface", "com.apple.iokit.IOGraphicsFamily",
                  "com.apple.AGXFirmwareKextRTBuddy64", "com.apple.AGXFirmwareKextG14XRTBuddy",
                  "com.apple.driver.IODARTFamily", "com.apple.driver.AppleT8110DART"],
    "video_display": ["com.apple.driver.AppleDCP", "com.apple.iokit.IOMobileGraphicsFamily",
                      "com.apple.iokit.IOMobileGraphicsFamily-DCP",
                      "com.apple.driver.AppleMobileDispT602X-DCP",
                      "com.apple.driver.AppleDisplayCrossbar", "com.apple.driver.AppleDPDisplayTCON",
                      "com.apple.driver.DCPAVFamilyProxy", "com.apple.driver.DCPDPFamilyProxy",
                      "com.apple.driver.AppleDCPDPTXProxy", "com.apple.iokit.IODisplayPortFamily",
                      "com.apple.iokit.IOHDCPFamily", "com.apple.driver.AppleSEPHDCPManager"],
    "video_media": ["com.apple.driver.AppleAVD", "com.apple.driver.AppleAVE2",
                    "com.apple.driver.AppleProResHW", "com.apple.driver.AppleJPEGDriver",
                    "com.apple.driver.AppleM2ScalerCSCDriver"],
    "video_camera": ["com.apple.driver.AppleH13CameraInterface"],
    "audio_builtin": ["com.apple.driver.AppleMCA2-T602x", "com.apple.driver.AudioDMAController-T602x",
                      "com.apple.driver.AppleEmbeddedAudio", "com.apple.iokit.AppleARMIISAudio",
                      "com.apple.driver.AppleCSEmbeddedAudio", "com.apple.driver.AppleCS42L84Audio",
                      "com.apple.driver.AppleAudioClockLibs", "com.apple.driver.AppleEmbeddedAudioLibs",
                      "com.apple.driver.ExclavesAudioKext", "com.apple.driver.IISAudioIsolatedStreamECProxy"],
    "audio_aop": ["com.apple.driver.AppleAOPAudio", "com.apple.driver.AppleAOPVoiceTrigger"],
    "audio_usb": ["com.apple.driver.AppleUSBAudio"],
}


def symbols(path, defined):
    result = subprocess.run([NM, "--arch=arm64e", "-j", *( ["--extern-only"] if defined else [] ),
        "--defined-only" if defined else "--undefined-only", str(path)],
        text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(f"cannot inspect {path}: {result.stderr}")
    return set(result.stdout.splitlines())


def inventory(root):
    result = {}
    for path in sorted(root.rglob("Info.plist")):
        bundle = path.parent.parent if path.parent.name == "Contents" else path.parent
        if bundle.suffix != ".kext":
            continue
        data = plistlib.loads(path.read_bytes())
        identifier = data.get("CFBundleIdentifier")
        if not identifier:
            continue
        if identifier in result:
            raise ValueError(f"duplicate bundle identifier {identifier}")
        executable = data.get("CFBundleExecutable")
        result[identifier] = {
            "bundle": str(bundle), "version": data.get("CFBundleVersion"),
            "dependencies": data.get("OSBundleLibraries_arm64e",
                data.get("OSBundleLibraries_arm64", data.get("OSBundleLibraries", {}))),
            "dependency_key": next((key for key in
                ("OSBundleLibraries_arm64e", "OSBundleLibraries_arm64", "OSBundleLibraries")
                if key in data), None),
            "executable": str((path.parent / "MacOS" / executable) if path.parent.name == "Contents"
                              else (path.parent / executable)) if executable else None,
        }
    return result


def closure(roots, bundles):
    visited, missing, kernel_components = set(), set(), set()
    def visit(identifier):
        if identifier in visited or identifier in kernel_components:
            return
        if identifier.startswith("com.apple.kpi.") or identifier == "com.apple.kernel":
            kernel_components.add(identifier)
            return
        if identifier not in bundles:
            missing.add(identifier)
            return
        visited.add(identifier)
        for dependency in bundles[identifier]["dependencies"]:
            visit(dependency)
    for identifier in roots:
        visit(identifier)
    return sorted(visited), sorted(missing), sorted(kernel_components)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def unresolved_imports(imports, providers, kernel):
    return sorted(imports - providers - kernel)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=[*PROFILES, "stock_loaded", "stock_boot", "all"], default="all")
    args = parser.parse_args()
    dependency = json.loads((OUT / "prepared-dependencies.json").read_text())
    kdk = Path(dependency["kdk_root"])
    custom = OUT / "xnu/BUILD/obj/DEVELOPMENT_ARM64_T6020/kernel.development.t6020.unstripped"
    stock = kdk / "System/Library/Kernels/kernel.development.t6020"
    bundles = inventory(kdk / "System/Library/Extensions")
    custom_exports, stock_exports = symbols(custom, True), symbols(stock, True)
    profiles = dict(PROFILES)
    if args.profile in ("all", "stock_loaded"):
        snapshot = json.loads((OUT / "driver-check/stock-inventory.json").read_text())
        if snapshot["product_build"] != "25F80":
            raise ValueError("stock driver inventory does not match the pinned build")
        profiles["stock_loaded"] = [x["identifier"] for x in snapshot["loaded_drivers"]]
    if args.profile in ("all", "stock_boot"):
        snapshot = json.loads((OUT / "driver-check/stock-collection.json").read_text())
        if snapshot["product_build"] != "25F80":
            raise ValueError("stock boot collection does not match the pinned build")
        profiles["stock_boot"] = [x["identifier"] for x in snapshot["entries"]
                                  if x["identifier"] != "com.apple.kernel"]
    if args.profile != "all":
        profiles = {args.profile: profiles[args.profile]}
    selections = {name: closure(roots, bundles) for name, roots in profiles.items()}
    cache, inspection_errors = {}, {}
    def inspect(identifier):
        executable = bundles[identifier]["executable"]
        if not executable:
            return identifier, None, None
        path = Path(executable)
        try:
            return identifier, (symbols(path, True), symbols(path, False), digest(path)), None
        except RuntimeError as error:
            return identifier, None, str(error)
    all_ids = sorted({identifier for ids, _, _ in selections.values() for identifier in ids})
    with ThreadPoolExecutor(max_workers=8) as pool:
        for identifier, data, error in pool.map(inspect, all_ids):
            if data is not None:
                cache[identifier] = data
            if error:
                inspection_errors[identifier] = error
    report = {"kernel": str(custom), "kernel_sha256": digest(custom),
              "control_kernel": str(stock), "control_sha256": digest(stock),
              "scope": "dependency closure and static symbol-name resolution only",
              "kernel_collection_linked": False, "runtime_driver_support_proven": False,
              "profiles": {}}
    passed = True
    for name, roots in profiles.items():
        ids, missing, components = selections[name]
        providers = set()
        errors = [inspection_errors[identifier] for identifier in ids if identifier in inspection_errors]
        for identifier in ids:
            executable = bundles[identifier]["executable"]
            if not executable:
                continue
            if identifier in cache:
                providers |= cache[identifier][0]
        details = []
        for identifier in ids:
            entry = dict(identifier=identifier, **bundles[identifier])
            if identifier in cache:
                imports = cache[identifier][1]
                unresolved = unresolved_imports(imports, providers, custom_exports)
                control_unresolved = unresolved_imports(imports, providers, stock_exports)
                entry.update(import_count=len(imports), unresolved=unresolved,
                             executable_sha256=cache[identifier][2],
                             control_unresolved=control_unresolved,
                             newly_unresolved=sorted(set(unresolved) - set(control_unresolved)))
            details.append(entry)
        unresolved_count = sum(len(x.get("unresolved", [])) for x in details)
        new_count = sum(len(x.get("newly_unresolved", [])) for x in details)
        ok = not missing and not errors and unresolved_count == 0
        passed &= ok
        report["profiles"][name] = {"roots": roots, "dependency_count": len(ids),
            "missing_dependencies": missing, "kernel_components": components,
            "inspection_errors": errors, "unresolved_import_count": unresolved_count,
            "newly_unresolved_import_count": new_count, "static_symbol_gate_passed": ok,
            "bundles": details}
        print(f"{name}: {len(ids)} bundles; {len(missing)} missing dependencies; "
              f"{unresolved_count} unresolved imports ({new_count} additional versus Apple kernel)")
    report["static_symbol_gate_passed"] = passed
    target = OUT / "driver-check"
    target.mkdir(exist_ok=True)
    (target / "symbol-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Report: {target / 'symbol-report.json'}")
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
