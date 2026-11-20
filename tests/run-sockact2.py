#!/usr/bin/env python3
"""Regression: multi-fd systemd socket activation survives the shim's execve.

Builds a disposable ARM64 guest that creates TWO bound+listening AF_UNIX sockets
(fds 3 and 4), sets LISTEN_FDS=2 / LISTEN_FDNAMES, execve()s a second program, and
asserts both fds 3 and 4 are valid sockets in the child, LISTEN_FDS/LISTEN_PID/
LISTEN_FDNAMES all reach the child, and an fd marked SOCK_CLOEXEC was NOT inherited.
"""
import os
import resource
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
(repo / "build").mkdir(exist_ok=True)

with tempfile.TemporaryDirectory(prefix="sockact2-", dir=repo / "build") as tmp:
    root = Path(tmp)
    (root / "bin").mkdir()
    (root / "tmp").mkdir()
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-sockact2.c"),
        "-o", str(root / "bin/probe"),
    ], check=True)

    group = tempfile.mkdtemp(prefix="rd-sockact2-", dir="/tmp")

    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    env = os.environ.copy()
    env["RDUID0"] = "1"
    env["RDSOCKETGROUP"] = group
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core, env=env)
    if result.returncode or "SOCKACT2_OK" not in result.stdout:
        raise SystemExit(f"sockact2 failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr[-2000:]}")
    print(result.stdout.strip())
