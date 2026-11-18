#!/usr/bin/env python3
"""Run static ARM64 msync probe against a disposable guest mapped file."""
import os
import resource
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
(repo / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="msync227-", dir=repo / "build") as tmp:
    root = Path(tmp)
    (root / "bin").mkdir()
    data = root / "data"
    data.write_bytes(b"\0" * 32768)
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-msync.c"),
        "-o", str(root / "bin/probe"),
    ], check=True)

    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core, env=os.environ.copy())
    if result.returncode or "GUEST_MSYNC_OK" not in result.stdout:
        raise SystemExit(f"msync guest failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr}")
    if data.read_bytes()[0] != ord("Q"):
        raise SystemExit("msync guest reported success without visible file update")
    print(result.stdout.strip())
