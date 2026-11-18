#!/usr/bin/env python3
"""Check ARM64 readlinkat path/output memory in a disposable guest root."""
import os
import resource
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
(repo / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="readlinkat78-", dir=repo / "build") as temp:
    root = Path(temp)
    (root / "bin").mkdir()
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-readlinkat-checked.c"),
        "-o", str(root / "bin/probe"),
    ], check=True)
    (root / "host-link").symlink_to(root / "target")

    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=20,
                            preexec_fn=no_core, env=os.environ.copy())
    if result.returncode or "READLINKAT_CHECKED_OK" not in result.stdout:
        raise SystemExit(f"readlinkat guest failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr}")
    print(result.stdout.strip())
