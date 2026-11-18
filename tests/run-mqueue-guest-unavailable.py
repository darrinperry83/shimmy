#!/usr/bin/env python3
"""Verify the current mqueue ABI remains honestly unavailable in a disposable ARM64 guest."""
from pathlib import Path
import resource
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


with tempfile.TemporaryDirectory(prefix="shimmy-mqueue-guest-") as temp:
    root = Path(temp)
    (root / "bin").mkdir()
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", "-O2",
        str(repo / "tests/guest-mqueue-unavailable.c"), "-o", str(root / "bin/probe"),
    ], check=True, timeout=30)
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=30, preexec_fn=no_core)
    if result.returncode or "GUEST_MQUEUE_UNAVAILABLE_OK" not in result.stdout:
        raise SystemExit(f"mqueue unavailable guard failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr[-1600:]}")
print("GUEST_MQUEUE_UNAVAILABLE_OK")
