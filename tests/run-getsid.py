#!/usr/bin/env python3
"""Exercise bounded getsid semantics without looking up unrelated host PIDs."""
import os
import resource
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
(repo / "build").mkdir(exist_ok=True)

def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

with tempfile.TemporaryDirectory(prefix="getsid156-", dir=repo / "build") as tmp:
    root = Path(tmp) / "root"
    (root / "bin").mkdir(parents=True)
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", f"-DHOST_PID={os.getpid()}",
        str(repo / "tests/guest-getsid.c"), "-o", str(root / "bin" / "probe"),
    ], check=True)
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=20, preexec_fn=no_core)
    if result.returncode or result.stdout.count("GUEST_GETSID_OK") != 2:
        raise SystemExit(f"getsid probe failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr}")
print("GUEST_GETSID_OK self, child, and unrelated host PID")
