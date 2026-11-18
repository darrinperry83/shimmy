#!/usr/bin/env python3
"""Probe invalid PUNCH_HOLE modes without risking a real file."""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="fallocate-punch-mode47-") as temp:
    root = Path(temp)
    (root / "bin").mkdir()
    (root / "tmp").mkdir()
    (root / "tmp/data").write_bytes(b"ABCDEFGH")
    guest = root / "bin/probe"
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start",
        str(repo / "tests/guest-fallocate-punch-mode.c"), "-o", str(guest),
    ], check=True, timeout=30)
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core)
    if result.returncode or result.stdout != "FALLOCATE_PUNCH_MODE_OK\n":
        raise RuntimeError(f"fallocate probe failed rc={result.returncode}:\n"
                           f"{result.stdout}\n{result.stderr[-1200:]}")
    print("FALLOCATE_PUNCH_MODE_OK invalid modes leave file unchanged; zero range works")
