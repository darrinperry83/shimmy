#!/usr/bin/env python3
"""Build and run a disposable AArch64 setrlimit guest probe."""
from pathlib import Path
import resource
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent

def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

with tempfile.TemporaryDirectory(prefix="shimmy-setrlimit-") as tmp:
    root = Path(tmp)
    (root / "bin").mkdir()
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start",
        str(repo / "tests/guest-setrlimit.c"), "-o", str(root / "bin/probe"),
    ], check=True)
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=20,
                            preexec_fn=no_core)
    if result.returncode or "GUEST_SETRLIMIT_OK" not in result.stdout:
        raise SystemExit(f"setrlimit probe failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr[-1800:]}")
print("GUEST_SETRLIMIT_OK")
