#!/usr/bin/env python3
"""Probe clone3 pointer safety and child paths in a disposable guest."""
from pathlib import Path
import os
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="clone3-input435-") as temp:
    root = Path(temp)
    (root / "bin").mkdir()
    guest = root / "bin/probe"
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start",
        str(repo / "tests/guest-clone3-debug-copyin.c"), "-o", str(guest),
    ], check=True, timeout=30)
    env = os.environ.copy()
    env["RDCLONEDBG"] = "1"
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=40,
                            preexec_fn=no_core, env=env)
    if result.returncode or result.stdout != "CLONE3_INPUT_OK\n":
        raise RuntimeError(f"clone3 input probe failed rc={result.returncode}:\n"
                           f"{result.stdout}\n{result.stderr[-1200:]}")
    print("CLONE3_INPUT_OK bad input/output, no child on EFAULT, fork and pidfd")
