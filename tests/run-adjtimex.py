#!/usr/bin/env python3
"""Exercise the read-only Linux ARM64 adjtimex query in a disposable root."""
import resource
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent

def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

with tempfile.TemporaryDirectory(prefix="shimmy-adjtimex-") as temp:
    root = Path(temp)
    (root / "bin").mkdir()
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", "-O2",
        str(repo / "tests/guest-adjtimex.c"), "-o", str(root / "bin/probe"),
    ], check=True, timeout=30)
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=30, preexec_fn=no_core)
    if result.returncode or "GUEST_ADJTIMEX_QUERY_OK" not in result.stdout:
        raise SystemExit(f"adjtimex probe failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr[-1600:]}")
print("GUEST_ADJTIMEX_QUERY_OK")
