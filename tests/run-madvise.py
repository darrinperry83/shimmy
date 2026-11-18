#!/usr/bin/env python3
"""Build and run a disposable ARM64 madvise guest probe."""
from pathlib import Path
import resource
import subprocess
import tempfile

base = Path(__file__).resolve().parent.parent
build = base / "build"
build.mkdir(exist_ok=True)

def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

with tempfile.TemporaryDirectory(prefix="madvise-", dir=build) as temporary:
    root = Path(temporary)
    (root / "bin").mkdir()
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(base / "tests/guest-madvise.c"),
        "-o", str(root / "bin/probe"),
    ], check=True, timeout=30)
    result = subprocess.run([str(base / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core)
    if result.returncode or "MADVISE_BOUNDED_OK" not in result.stdout:
        raise RuntimeError(f"guest madvise probe failed: {result.returncode}\n"
                           f"{result.stdout}\n{result.stderr[-1500:]}")
    print("MADVISE_BOUNDED_OK")
