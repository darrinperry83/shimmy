#!/usr/bin/env python3
"""seccomp() must accept systemd's large @system-service filter and answer
SECCOMP_GET_ACTION_AVAIL, or hardened services (polkit, upower) die at "step
SECCOMP" instead of starting, which fails D-Bus activation.
"""
import os
import resource
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
(repo / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="seccomp-filter-", dir=repo / "build") as tmp:
    root = Path(tmp)
    (root / "tmp").mkdir()
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-seccomp-filter.c"),
        "-o", str(root / "probe"),
    ], check=True)

    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    env = os.environ.copy()
    env["RDUID0"] = "1"
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core, env=env)
    if result.returncode or "SECCOMP_FILTER_OK" not in result.stdout:
        raise SystemExit(f"seccomp-filter guest failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr[-2000:]}")
    print(result.stdout.strip())
