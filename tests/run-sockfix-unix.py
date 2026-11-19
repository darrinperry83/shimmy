#!/usr/bin/env python3
"""Regression: systemd socket activation on a non-/tmp AF_UNIX path.

Builds a disposable root whose /etc/pacman.d is read-only and proves the whole
bind sequence on /etc/pacman.d/gnupg/S.test succeeds because the shim relocates
the socket and its parent directory into the per-boot socket group.
"""
import os
import resource
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
(repo / "build").mkdir(exist_ok=True)

with tempfile.TemporaryDirectory(prefix="sockfix-unix-", dir=repo / "build") as tmp:
    root = Path(tmp)
    (root / "bin").mkdir()
    (root / "etc" / "pacman.d").mkdir(parents=True)
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-sockfix-unix.c"),
        "-o", str(root / "bin/probe"),
    ], check=True)
    # Read-only parent: the rootfs copy of /etc/pacman.d/gnupg cannot be created,
    # exactly like a boot with a read-only /etc.
    (root / "etc" / "pacman.d").chmod(0o555)

    group = tempfile.mkdtemp(prefix="rd-sockfix-unix-", dir="/tmp")

    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    env = os.environ.copy()
    env["RDUID0"] = "1"
    env["RDSOCKETGROUP"] = group
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core, env=env)
    if result.returncode or "SOCKFIX_OK" not in result.stdout:
        raise SystemExit(f"sockfix-unix failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr[-2000:]}")
    print(result.stdout.strip())
