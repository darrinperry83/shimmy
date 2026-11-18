#!/usr/bin/env python3
"""Build and run an ARM64 semctl refusal probe without touching host IPC."""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="semctl191-") as temp:
    root = Path(temp)
    (root / "bin").mkdir()
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-semctl-refusal.c"),
        "-o", str(root / "bin/probe"),
    ], check=True, timeout=30)
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core)
    if result.returncode or "SEMCTL_REFUSAL_OK" not in result.stdout:
        raise RuntimeError(f"semctl guest failed ({result.returncode}):\n"
                           f"{result.stdout}\n{result.stderr[-1200:]}")
    print("SEMCTL_REFUSAL_OK")
