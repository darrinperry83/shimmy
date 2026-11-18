#!/usr/bin/env python3
"""Test ARM64 lseek against synthetic getdents64 and an ordinary file."""
import os
import resource
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
(repo / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="lseek62-", dir=repo / "build") as temp:
    root = Path(temp)
    (root / "bin").mkdir()
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-lseek-synth-dir.c"),
        "-o", str(root / "bin/probe"),
    ], check=True)

    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core, env=os.environ.copy())
    if result.returncode or "LSEEK_SYNTH_DIR_OK" not in result.stdout:
        raise SystemExit(f"lseek guest failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr}")
    print(result.stdout.strip())
