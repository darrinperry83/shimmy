#!/usr/bin/env python3
"""Build and run a disposable ARM64 semget refusal test without host IPC."""
from pathlib import Path
import resource
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
(repo / "build").mkdir(exist_ok=True)


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


with tempfile.TemporaryDirectory(prefix="semget190-", dir=repo / "build") as temp:
    root = Path(temp)
    (root / "bin").mkdir()
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-semget-refusal.c"),
        "-o", str(root / "bin/probe"),
    ], check=True, timeout=30)
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core)
    if result.returncode or "SEMGET_REFUSAL_OK" not in result.stdout:
        raise RuntimeError(f"semget guest failed ({result.returncode}):\n"
                           f"{result.stdout}\n{result.stderr[-1200:]}")
    print("SEMGET_REFUSAL_OK")
