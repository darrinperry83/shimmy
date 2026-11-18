#!/usr/bin/env python3
"""Probe the narrow #179 guest-output boundary in a disposable ARM64 guest."""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="sysinfo-copyout179-") as temp:
    root = Path(temp)
    (root / "bin").mkdir()
    guest = root / "bin/probe"
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start",
        str(repo / "tests/guest-sysinfo-copyout.c"), "-o", str(guest),
    ], check=True, timeout=30)
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core)
    if result.returncode or result.stdout != "SYSINFO_COPYOUT_OK\n":
        raise RuntimeError(f"sysinfo probe failed rc={result.returncode}:\n"
                           f"{result.stdout}\n{result.stderr[-1200:]}")
    print("SYSINFO_COPYOUT_OK existing fields, guard, NULL and bad pointer")
