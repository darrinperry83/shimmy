#!/usr/bin/env python3
"""Run disposable no-libc ARM64 setsockopt guest-input probe."""
from pathlib import Path
import os
import resource
import subprocess
import tempfile

base = Path(__file__).resolve().parent.parent
(base / "build").mkdir(exist_ok=True)

def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

with tempfile.TemporaryDirectory(prefix="setsockopt-input-", dir=base / "build") as temp:
    root = Path(temp)
    (root / "bin").mkdir()
    subprocess.run(["clang", "--target=aarch64-linux-gnu", "-nostdlib",
                    "-static-pie", "-fuse-ld=lld", "-Wl,-e,_start",
                    str(base / "tests/guest-setsockopt-input.c"),
                    "-o", str(root / "bin/probe")], check=True, timeout=30)
    result = subprocess.run([str(base / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core, env=os.environ.copy())
    if result.returncode or "SETSOCKOPT_INPUT_OK" not in result.stdout:
        raise RuntimeError(f"exit={result.returncode}\n{result.stdout}\n{result.stderr[-1800:]}")
    print("SETSOCKOPT_INPUT_OK")
