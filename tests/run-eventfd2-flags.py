#!/usr/bin/env python3
"""Probe eventfd2 flag admission in a disposable AArch64 guest."""
from pathlib import Path
import os
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="eventfd2-flags19-") as temp:
    root = Path(temp)
    (root / "bin").mkdir()
    guest = root / "bin/probe"
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start",
        str(repo / "tests/guest-eventfd2-flags.c"), "-o", str(guest),
    ], check=True, timeout=30)
    with tempfile.TemporaryDirectory(prefix="e19-", dir="/tmp") as group:
        env = os.environ.copy()
        env["RDSOCKETGROUP"] = group
        result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                                capture_output=True, text=True, timeout=30,
                                preexec_fn=no_core, env=env)
    if result.returncode or result.stdout != "EVENTFD2_FLAGS_OK\n":
        raise RuntimeError(f"eventfd2 probe failed rc={result.returncode}:\n"
                           f"{result.stdout}\n{result.stderr[-1200:]}")
    print("EVENTFD2_FLAGS_OK invalid and semaphore refusals; supported fd flags")
