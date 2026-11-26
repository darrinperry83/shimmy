#!/usr/bin/env python3
"""Build and verify P0 locally. Never changes boot settings or runs the positive probe."""
import argparse
import hashlib
import json
import platform
import subprocess
import tempfile
import urllib.request
from pathlib import Path
import prepare

HERE = Path(__file__).resolve().parent
OUT = HERE / "out"


def run(*args, quiet=False):
    result = subprocess.run(args, check=True, text=True, capture_output=True)
    if result.stdout and not quiet:
        print(result.stdout, end="")
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-upstream", action="store_true")
    parser.add_argument("--stock-control", action="store_true",
                        help="run an isolated negative control on an unpatched kernel")
    args = parser.parse_args()
    OUT.mkdir(exist_ok=True)
    checks = []
    for mode in ("DEVELOPMENT", "RELEASE"):
        binary = OUT / f"test-{mode.lower()}"
        run("clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-function", f"-D{mode}=1", "-I", str(HERE),
            str(HERE / "test_dispatch.c"), "-o", str(binary))
        run(str(binary))
        checks.append(f"host_dispatch_model_{mode.lower()}")
    if args.check_upstream:
        url = f"https://raw.githubusercontent.com/apple-oss-distributions/xnu/{prepare.COMMIT}/{prepare.SLEH}"
        with urllib.request.urlopen(url, timeout=30) as response:
            original = response.read()
        patched = prepare.patched(original)
        assert patched.count("if (p && rdxnu_linux_probe(state, p))") == 1
        assert patched.count('#include "rdxnu_linux_probe.h"') == 1
        # Exercise --apply, and ensure a second application is refused.
        with tempfile.TemporaryDirectory(prefix="rdxnu-prepare-") as temp:
            root = Path(temp)
            target = root / prepare.SLEH
            target.parent.mkdir(parents=True)
            target.write_bytes(original)
            run("python3", str(HERE / "prepare.py"), str(root), "--apply", quiet=True)
            assert target.read_text() == patched
            assert target.with_name("rdxnu_linux_probe.h").read_bytes() == (HERE / "rdxnu_linux_probe.h").read_bytes()
            retry = subprocess.run(["python3", str(HERE / "prepare.py"), str(root), "--apply"],
                                   capture_output=True)
            assert retry.returncode != 0
        for invalid in (original + b"\n", original.replace(b"handle_svc", b"different", 1)):
            try:
                prepare.patched(invalid)
            except ValueError:
                pass
            else:
                raise AssertionError("accepted unpinned source")
        (OUT / "sleh.patch").write_text("".join(prepare.difflib.unified_diff(
            original.decode().splitlines(True), patched.splitlines(True),
            fromfile=f"a/{prepare.SLEH}", tofile=f"b/{prepare.SLEH}")))
        checks.append("exact_upstream_source_patch_and_refusal_controls")
        print("UPSTREAM_PATCH_PASS: exact pin, application, repeat/mismatch refusal")
    if platform.system() == "Darwin" and platform.machine() == "arm64":
        binary = OUT / "probe"
        run("xcrun", "clang", "-arch", "arm64", "-O2", "-Wall", "-Wextra", "-Werror",
            str(HERE / "probe.c"), str(HERE / "probe_syscall.S"), "-o", str(binary))
        disassembly = run("otool", "-tvV", str(binary), quiet=True)
        (OUT / "probe-disassembly.txt").write_text(disassembly)
        assert "svc\t#0" in disassembly or "svc\t#0x0" in disassembly
        checks.append("native_arm64_probe_build_and_svc_instruction")
        print("NATIVE_PROBE_BUILD_PASS: actual svc #0 present")
        if args.stock_control:
            run(str(binary), "--expect-stock")
            checks.append("stock_kernel_negative_control")
    elif args.stock_control:
        raise RuntimeError("stock control requires an Apple Silicon Mac")
    report = {"xnu_tag": prepare.TAG, "xnu_commit": prepare.COMMIT,
              "host_kernel": platform.release(), "checks_passed": checks,
              "full_xnu_compiled": False, "modified_kernel_booted": False,
              "kernel_linux_abi_proven": False,
              "probe_assembly_sha256": hashlib.sha256((HERE / "probe_syscall.S").read_bytes()).hexdigest(),
              "kernel_header_sha256": hashlib.sha256((HERE / "rdxnu_linux_probe.h").read_bytes()).hexdigest()}
    audit_path = OUT / "kernel-audit.json"
    if audit_path.exists():
        audit = json.loads(audit_path.read_text())
        kernel = Path(audit["kernel"])
        with kernel.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        if digest != audit["sha256"] or report["kernel_header_sha256"] != audit["kernel_header_sha256"]:
            raise RuntimeError("kernel audit is stale; rebuild and audit before verification")
        report["full_xnu_compiled"] = audit["full_xnu_compiled"]
        report["linked_kernel_sha256"] = digest
    (OUT / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
    print("LOCAL_VERIFICATION_PASS; kernel execution remains unproven")


if __name__ == "__main__":
    main()
