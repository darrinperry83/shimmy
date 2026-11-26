#!/usr/bin/env python3
"""Inspect a linked P0 kernel. This does not establish that it boots."""
import hashlib
import json
from pathlib import Path
import re
import subprocess

HERE = Path(__file__).resolve().parent
OUT = HERE / "out"
KERNEL = OUT / "xnu/BUILD/obj/DEVELOPMENT_ARM64_T6020/kernel.development.t6020"


def capture(*args):
    return subprocess.check_output(list(map(str, args)), text=True)


def audit(kernel=KERNEL, output=OUT):
    kernel = Path(kernel)
    output = Path(output)
    unstripped = kernel.with_name(kernel.name + ".unstripped")
    description = capture("file", kernel)
    if "Mach-O 64-bit executable arm64e" not in description:
        raise ValueError("expected an arm64e Mach-O kernel")
    if capture("xcrun", "llvm-nm", "--undefined-only", unstripped).strip():
        raise ValueError("kernel has unresolved symbols")
    disassembly = capture("xcrun", "llvm-objdump", "-d",
                          "--disassemble-symbols=_sleh_synchronous", unstripped)
    checks = {
        "opt_in_marker": "#0x4c58" in disassembly,
        "linux_getpid_number": "#0xac" in disassembly,
        "linux_negative_enosys": "#-0x26" in disassembly,
        "real_proc_pid_call": "<_proc_pid>" in disassembly,
        "boot_argument_check": "<_PE_parse_boot_argn_internal>" in disassembly,
        "boot_argument_string": b"rdxnu_linux_probe\x00" in kernel.read_bytes(),
    }
    if not all(checks.values()):
        raise ValueError(f"missing kernel experiment evidence: {checks}")
    uuid_output = capture("xcrun", "dwarfdump", "--uuid", kernel)
    match = re.search(r"UUID: ([A-F0-9-]+) \(arm64e\)", uuid_output)
    if not match:
        raise ValueError("kernel UUID unavailable")
    if f"UUID: {match.group(1)} (arm64e)" not in capture("xcrun", "dwarfdump", "--uuid", unstripped):
        raise ValueError("stripped and unstripped kernels come from different builds")
    with kernel.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    report = {"kernel": str(kernel), "sha256": digest, "uuid": match.group(1),
              "size": kernel.stat().st_size, "architecture": "arm64e",
              "checks": checks, "full_xnu_compiled": True,
              "kernel_header_sha256": hashlib.sha256((HERE / "rdxnu_linux_probe.h").read_bytes()).hexdigest(),
              "modified_kernel_booted": False, "kernel_linux_abi_proven": False,
              "driver_compatibility_proven": False}
    (output / "svc-disassembly.txt").write_text(disassembly)
    (output / "kernel-audit.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"LINKED_KERNEL_AUDIT_PASS: {digest}; UUID {match.group(1)}")
    return report


if __name__ == "__main__":
    audit()
