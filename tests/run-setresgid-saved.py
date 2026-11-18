#!/usr/bin/env python3
"""Build and run a disposable ARM64 saved-GID refusal probe."""
import os
import resource
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
(repo / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="setresgid149-", dir=repo / "build") as tmp:
    root = Path(tmp)
    (root / "bin").mkdir()
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-setresgid-saved.c"),
        "-o", str(root / "bin/probe"),
    ], check=True)

    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    env = os.environ.copy()
    env.pop("RDUID0", None)
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=20,
                            preexec_fn=no_core, env=env)
    if result.returncode or "GUEST_SETRESGID_SAVED_OK" not in result.stdout:
        raise SystemExit(f"setresgid guest failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr[-2000:]}")
    print(result.stdout.strip())
