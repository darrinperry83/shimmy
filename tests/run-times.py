#!/usr/bin/env python3
"""Build and run a disposable static AArch64 times(2) probe."""
import os
from pathlib import Path
import resource
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


with tempfile.TemporaryDirectory(prefix="shimmy-times-") as tmp:
    root = Path(tmp)
    (root / "bin").mkdir()
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-times.c"),
        "-o", str(root / "bin/probe"),
    ], check=True)
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=20,
                            preexec_fn=no_core, env=os.environ.copy())
    if result.returncode or "GUEST_TIMES_OK" not in result.stdout:
        raise SystemExit(f"times guest failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr[-2000:]}")
    print(result.stdout.strip())
