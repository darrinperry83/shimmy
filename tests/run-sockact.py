#!/usr/bin/env python3
"""Regression: systemd socket activation survives the shim's execve.

Builds a disposable ARM64 guest that creates a bound+listening AF_UNIX socket
without SOCK_CLOEXEC, sets LISTEN_FDS=1, execve()s a second program (passing the
socket as fd 3), and asserts the child can getsockname()/fstat it and that an fd
marked SOCK_CLOEXEC was NOT inherited.
"""
import os
import resource
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
(repo / "build").mkdir(exist_ok=True)

with tempfile.TemporaryDirectory(prefix="sockact-", dir=repo / "build") as tmp:
    root = Path(tmp)
    (root / "bin").mkdir()
    (root / "tmp").mkdir()
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-sockact.c"),
        "-o", str(root / "bin/probe"),
    ], check=True)

    group = tempfile.mkdtemp(prefix="rd-sockact-", dir="/tmp")

    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    env = os.environ.copy()
    env["RDUID0"] = "1"
    env["RDSOCKETGROUP"] = group
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core, env=env)
    if result.returncode or "SOCKACT_OK" not in result.stdout:
        raise SystemExit(f"sockact failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr[-2000:]}")
    print(result.stdout.strip())
