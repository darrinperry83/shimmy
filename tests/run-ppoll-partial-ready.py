#!/usr/bin/env python3
"""Verify ppoll returns ready descriptors without waiting on idle sockets."""
import os
import resource
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
(repo / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="autovt-", dir=repo / "build") as tmp:
    root = Path(tmp)
    (root / "bin").mkdir()
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-ppoll-partial-ready.c"),
        "-o", str(root / "bin/probe"),
    ], check=True)

    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    env = os.environ.copy()
    env["RDUID0"] = "1"
    env["RDVT"] = "1"
    result = subprocess.run([os.environ.get("RD_TEST_BINARY", str(repo / "shim_cli")), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=3,
                            preexec_fn=no_core, env=env)
    if result.returncode or "PPOLL_PARTIAL_READY_OK" not in result.stdout:
        raise SystemExit(f"ppoll partial ready failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr[-2000:]}")
    print(result.stdout.strip())
