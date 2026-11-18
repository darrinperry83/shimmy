#!/usr/bin/env python3
"""Compile and run a disposable ARM64 sched_get_priority_max probe."""
from pathlib import Path
import resource
import subprocess
import tempfile

base = Path(__file__).resolve().parent.parent
build = base / "build"
build.mkdir(exist_ok=True)

def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

with tempfile.TemporaryDirectory(prefix="sched-priority-max-", dir=build) as temporary:
    root = Path(temporary)
    (root / "bin").mkdir()
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(base / "tests/guest-sched-priority-max.c"),
        "-o", str(root / "bin/probe"),
    ], check=True, timeout=30)
    result = subprocess.run([str(base / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core)
    if result.returncode:
        raise RuntimeError(f"sched priority max probe exit {result.returncode}:\n"
                           f"{result.stdout}\n{result.stderr[-1500:]}")
    print("SCHED_PRIORITY_MAX_OK")
