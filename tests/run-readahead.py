#!/usr/bin/env python3
"""Run a disposable ARM64 guest probe for the readahead syscall."""
from pathlib import Path
import resource
import subprocess
import tempfile

base = Path(__file__).resolve().parent.parent
build = base / "build"
build.mkdir(exist_ok=True)

def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

with tempfile.TemporaryDirectory(prefix="readahead-", dir=build) as temporary:
    root = Path(temporary)
    (root / "bin").mkdir()
    (root / "bin/sample").write_bytes(b"x" * 128)
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(base / "tests/guest-readahead.c"),
        "-o", str(root / "bin/probe"),
    ], check=True, timeout=30)
    result = subprocess.run([str(base / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core)
    if result.returncode:
        raise RuntimeError(f"readahead probe exit {result.returncode}:\n"
                           f"{result.stdout}\n{result.stderr[-1500:]}")
    print("READAHEAD_OK")
