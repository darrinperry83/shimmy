#!/usr/bin/env python3
"""Run raw ARM64 no-LSM set-attribute probe in a disposable root."""
import os
import resource
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
(repo / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="lsm460-", dir=repo / "build") as tmp:
    root = Path(tmp)
    (root / "bin").mkdir()
    subprocess.run(["clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
                    "-fuse-ld=lld", "-Wl,-e,_start",
                    str(repo / "tests/guest-lsm-set-self-attr.c"),
                    "-o", str(root / "bin/probe")], check=True)

    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=20,
                            preexec_fn=no_core, env=os.environ.copy())
    if result.returncode or "GUEST_LSM_SET_OK" not in result.stdout:
        raise SystemExit(f"LSM set guest failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr}")
    print(result.stdout.strip())
