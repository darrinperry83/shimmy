#!/usr/bin/env python3
"""Exercise bounded ARM64 clock_adjtime without touching host clock state."""
import resource
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent

def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

with tempfile.TemporaryDirectory(prefix="shimmy-clock-adjtime-") as temp:
    root = Path(temp)
    (root / "bin").mkdir()
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", "-O2",
        str(repo / "tests/guest-clock-adjtime.c"), "-o", str(root / "bin/probe"),
    ], check=True, timeout=30)
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=30, preexec_fn=no_core)
    if result.returncode or "GUEST_CLOCK_ADJTIME_OK" not in result.stdout:
        raise SystemExit(f"clock_adjtime probe failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr[-1600:]}")
print("GUEST_CLOCK_ADJTIME_OK")
