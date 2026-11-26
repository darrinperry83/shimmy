#!/usr/bin/env python3
"""Prepare/build the P0 kernel locally. Never installs or boots a kernel."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import prepare
import audit_kernel
import prepare_sep
import audit_sep
import vtable_gate

HERE = Path(__file__).resolve().parent
OUT = HERE / "out"
CONFIG = json.loads((HERE / "dependencies.json").read_text())


def run(*args, cwd=None, log=None, env=None):
    print("+", (f"{args[0]} ({len(args) - 1} arguments; log: {log})" if log
                 else " ".join(map(str, args))), flush=True)
    if log:
        with log.open("w") as stream:
            return subprocess.run(list(map(str, args)), cwd=cwd, env=env,
                                  stdout=stream, stderr=subprocess.STDOUT).returncode
    subprocess.run(list(map(str, args)), cwd=cwd, env=env, check=True)
    return 0


def sources():
    for name, (tag, commit) in CONFIG["sources"].items():
        path = OUT / name
        if not path.exists():
            run("git", "clone", "--depth", "1", "--branch", tag,
                f"https://github.com/apple-oss-distributions/{name}.git", path)
        actual = subprocess.check_output(["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()
        if actual != commit:
            raise ValueError(f"{name}: unexpected revision {actual}")


def extract_kdk():
    spec = CONFIG["kdk"]
    image = OUT / spec["filename"]
    if not image.exists():
        run("curl", "-fL", "--retry", "3", "--connect-timeout", "30",
            spec["url"], "-o", image)
    with image.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    if digest != spec["sha256"]:
        raise ValueError("KDK digest mismatch; refusing extraction")
    expanded = OUT / "kdk-expanded"
    receipt = OUT / "kdk-extraction.json"
    if not receipt.exists():
        if expanded.exists():
            raise ValueError("incomplete KDK extraction exists; inspect before retrying")
        mount = OUT / "kdk-volume"
        mount.mkdir(exist_ok=True)
        run("hdiutil", "attach", "-readonly", "-nobrowse", "-mountpoint", mount, image)
        try:
            package = mount / "KernelDebugKit.pkg"
            signature = subprocess.check_output(["pkgutil", "--check-signature", str(package)], text=True)
            print(signature, flush=True)
            if "Status: signed Apple Software" not in signature or "Apple Root CA" not in signature:
                raise ValueError("KDK package lacks expected trusted Apple signature")
            run("pkgutil", "--expand-full", package, expanded)
            receipt.write_text(json.dumps({"dmg_sha256": digest,
                                          "package_signature": signature}, indent=2) + "\n")
        finally:
            run("hdiutil", "detach", mount)
    elif json.loads(receipt.read_text())["dmg_sha256"] != digest:
        raise ValueError("KDK receipt does not match image")
    root = OUT / "KDK_26.5.1_25F80.kdk"
    complete = root / ".rdxnu-payloads-merged"
    if not complete.exists():
        for name in ("KDK.pkg", "KDK_SDK.pkg"):
            payload = expanded / name / "Payload"
            if not payload.is_dir():
                raise ValueError(f"missing KDK payload: {payload}")
            run("ditto", payload, root)
        complete.write_text(digest + "\n")
    elif complete.read_text().strip() != digest:
        raise ValueError("KDK merge receipt does not match image")
    return root


def prepare_source(kdk):
    xnu = OUT / "xnu"
    source = xnu / prepare.SLEH
    header = source.with_name("rdxnu_linux_probe.h")
    original = subprocess.check_output(["git", "-C", str(xnu), "show", f"HEAD:{prepare.SLEH}"])
    expected = prepare.patched(original)
    if source.read_bytes() == original:
        run(sys.executable, HERE / "prepare.py", xnu, "--apply")
    elif source.read_text() != expected or header.read_bytes() != (HERE / header.name).read_bytes():
        raise ValueError("unexpected SVC source edits; refusing to overwrite")
    private = kdk / "System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders"
    ce = xnu / "EXTERNAL_HEADERS/CoreEntitlements/V2"
    ce.mkdir(exist_ok=True)
    for path in (private / "platform/CoreEntitlements/V2").glob("*.h"):
        shutil.copy2(path, ce / path.name)
    kernel_h = kdk / "System/Library/KernelSupport/CoreEntitlements/V2/Kernel.h"
    if not kernel_h.is_file():
        raise ValueError("selected KDK does not contain CoreEntitlements Kernel.h")
    shutil.copy2(kernel_h, ce / "Kernel.h")
    # Install exactly the public project's kernel export of this header, rather
    # than a hand-written substitute or enabling its SPI throughout XNU.
    stage = OUT / "stage/usr/local/include/kernel/os"
    stage.mkdir(parents=True, exist_ok=True)
    exported = subprocess.run(["xcrun", "unifdef", "-DKERNEL=1", "-DOS_FIREHOSE_SPI=1",
        "-DOS_VOUCHER_ACTIVITY_SPI_TYPES=1", "-UOS_VOUCHER_ACTIVITY_SPI",
        str(OUT / "libdispatch/os/firehose_buffer_private.h")], capture_output=True)
    if exported.returncode not in (0, 1):
        raise RuntimeError(exported.stderr.decode())
    target = stage / "firehose_buffer_private.h"
    if not target.exists() or target.read_bytes() != exported.stdout:
        target.write_bytes(exported.stdout)
    prepare_sep.prepare(xnu)
    return private


def build_firehose(machine="T6020", artifacts=OUT):
    # Reuse a real libkern compilation's argument vector: same board, private
    # headers, PAC and kernel configuration as the kernel we are linking into.
    record = OUT / f"xnu/BUILD/obj/DEVELOPMENT_ARM64_{machine}/libkern/DEVELOPMENT/OSKextVersion.o.json"
    invocation = json.loads(record.read_text().rstrip(",\n"))
    command = invocation["arguments"][:]
    if f"ARM64_BOARD_CONFIG_{machine}" not in command or "arm64e" not in command:
        raise ValueError("unexpected libkern compile configuration")
    command[2] = str(OUT / "libdispatch/src/firehose/firehose_buffer.c")
    command[command.index("-o") + 1] = str(artifacts / "firehose_buffer.o")
    command += [f"-I{OUT / 'libdispatch'}", f"-I{OUT / 'xnu/libkern/firehose'}",
                "-DDISPATCH_USE_DTRACE=0", "-DOS_ATOMIC_CONFIG_MEMORY_ORDER_DEPENDENCY=1",
                "-DOS_ATOMIC_CONFIG_STARVATION_FREE_ONLY=0"]
    obj = artifacts / "firehose_buffer.o"
    prior = hashlib.sha256(obj.read_bytes()).hexdigest() if obj.exists() else None
    result = run(*command, cwd=invocation["directory"], log=artifacts / "firehose-build.log")
    if result:
        raise RuntimeError(f"real firehose source compilation failed; see {artifacts / 'firehose-build.log'}")
    if prior != hashlib.sha256(obj.read_bytes()).hexdigest() or not (artifacts / "libfirehose_kernel.a").exists():
        run("xcrun", "libtool", "-static", "-o", artifacts / "libfirehose_kernel.a", obj)
    (artifacts / "firehose-build-command.json").write_text(json.dumps(command, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prepare", action="store_true", help="fetch pinned dependencies and extract KDK")
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--machine", choices=("T6020", "VMAPPLE"), default="T6020")
    args = parser.parse_args()
    machine = args.machine
    artifacts = OUT if machine == "T6020" else OUT / "vmapple"
    if not args.prepare and not args.build:
        parser.error("choose --prepare and/or --build")
    OUT.mkdir(exist_ok=True)
    artifacts.mkdir(exist_ok=True)
    lock = (OUT / "kernel-build.lock").open("a")
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    sources()
    kdk = extract_kdk()
    private = prepare_source(kdk)
    sdk = OUT / "MacOSX26.5.sdk"
    if not sdk.exists():
        public_sdk = subprocess.check_output(["xcrun", "--sdk", "macosx", "--show-sdk-path"], text=True).strip()
        run(sys.executable, HERE / "sdk_overlay.py", public_sdk, OUT / "AvailabilityVersions", sdk)
    if json.loads((sdk / "SDKSettings.json").read_text()).get("Version") != "26.5":
        raise ValueError("this pinned build requires the macOS 26.5 SDK")
    print(f"KDK_ROOT={kdk}", flush=True)
    (OUT / "prepared-dependencies.json").write_text(json.dumps({
        "kdk_root": str(kdk), "private_headers": str(private),
        "config": CONFIG, "kernel_installed": False}, indent=2) + "\n")
    if not args.build:
        return
    includes = [OUT / "stage/usr/local/include/kernel", private, kdk / "System/Library/KernelSupport"]
    for root in (OUT / "libplatform/include", OUT / "libplatform/private", OUT / "libdispatch"):
        includes.append(root)
    platform_headers = [private / "platform"]
    public_platform = kdk / "System/Library/Frameworks/Kernel.framework/Versions/A/Headers/platform"
    if public_platform.is_dir():
        platform_headers.append(public_platform)
    if not all(p.is_dir() for p in includes + platform_headers):
        raise ValueError("required real header directory is missing")
    platform_path = subprocess.check_output(["xcrun", "--sdk", "macosx", "--show-sdk-platform-path"], text=True).strip()
    command = ["make", f"SDKROOT={sdk}", "SDKVERSION=26.5", "PLATFORM=MacOSX", f"PLATFORMPATH={platform_path}",
               "ARCH_CONFIGS=ARM64", f"MACHINE_CONFIGS={machine}", "KERNEL_CONFIGS=DEVELOPMENT",
               "RC_DARWIN_KERNEL_VERSION=25.5.0", f"KDKROOT={kdk}",
               f"LDFLAGS_KERNEL_SDK=-L{artifacts} -L{kdk / 'usr/local/lib/kernel/platform'}",
               "INCFLAGS_SDK=" + " ".join(f"-I{p}" for p in includes),
               "INCFLAGS_PLATFORM=" + " ".join(f"-I{p}" for p in platform_headers),
               "BUILD_WERROR=0", "BOUND_CHECKS=0",
               "CWARNFLAGS=-Wall -Wextra -Wno-unused-parameter",
               "CXXWARNFLAGS=-Wall -Wextra -Wno-unused-parameter", "MAKEJOBS=-j8"]
    log = artifacts / "kernel-kdk-build.log"
    result_file = artifacts / "kernel-build-result.json"
    report = {"command": command, "state": "building", "returncode": None,
              "log": str(log), "modified_kernel_booted": False,
              "driver_compatibility_proven": False}
    result_file.write_text(json.dumps(report, indent=2) + "\n")
    record = OUT / f"xnu/BUILD/obj/DEVELOPMENT_ARM64_{machine}/libkern/DEVELOPMENT/OSKextVersion.o.json"
    built_firehose = record.is_file()
    if built_firehose:
        build_firehose(machine, artifacts)
    result = run(*command, cwd=OUT / "xnu", log=log)
    if result and not built_firehose and record.is_file() and "firehose_kernel" in log.read_text(errors="replace"):
        # A fresh checkout needs one kernel pass to generate the real compiler
        # invocation. Preserve that first log and retry only after building it.
        shutil.copy2(log, artifacts / "kernel-before-firehose-build.log")
        build_firehose(machine, artifacts)
        result = run(*command, cwd=OUT / "xnu", log=log)
    report.update(state="built" if not result else "failed", returncode=result)
    if not result:
        try:
            kernel = OUT / f"xnu/BUILD/obj/DEVELOPMENT_ARM64_{machine}/kernel.development.{machine.lower()}"
            report["artifact"] = audit_kernel.audit(kernel, artifacts)
            if machine == "T6020":
                report["sep_interfaces"] = audit_sep.audit()
            else:
                report["vtables"] = vtable_gate.audit(machine)
                if not report["vtables"]["vtable_gate_passed"]:
                    raise ValueError("VMAPPLE vtable audit failed")
        except Exception as error:
            report.update(state="audit_failed", audit_error=str(error))
            result_file.write_text(json.dumps(report, indent=2) + "\n")
            raise
    result_file.write_text(json.dumps(report, indent=2) + "\n")
    print(f"KERNEL_BUILD_EXIT={result}; log: {log}", flush=True)
    sys.exit(result)


if __name__ == "__main__":
    main()
