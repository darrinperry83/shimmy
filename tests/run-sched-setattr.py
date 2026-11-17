#!/usr/bin/env python3
"""Run sched_setattr ABI proof in an isolated minimal AArch64 guest root."""
import os
import resource
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
build = repo / "build"
build.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="sched274-", dir=build) as tmp:
    root = Path(tmp)
    (root / "bin").mkdir()
    probe = root / "bin/probe"
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start",
        str(repo / "tests/guest-sched-setattr.c"), "-o", str(probe),
    ], check=True)

    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core, env=os.environ.copy())
    if result.returncode or "sched_setattr guest ABI PASS" not in result.stdout:
        raise SystemExit(f"guest failed ({result.returncode}):\n{result.stdout}{result.stderr}")
    print(result.stdout.strip())
