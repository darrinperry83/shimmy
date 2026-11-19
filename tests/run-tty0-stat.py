#!/usr/bin/env python3
"""Build and run a disposable ARM64 probe: /dev/tty0 must stat as a 4:0 char device.

systemd's getty@.service gates on ConditionPathExists=/dev/tty0, which is evaluated with
access(F_OK) (faccessat) but the node itself is synthesized by the shim's stat/statx path.
This proves statx(2), newfstatat(2) and faccessat(2) all see the node as a char device with
the right major/minor (see tests/guest-tty0-stat.c).
"""
import os
import resource
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
(repo / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="tty0-stat-", dir=repo / "build") as tmp:
    root = Path(tmp)
    (root / "tmp").mkdir()
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-tty0-stat.c"),
        "-o", str(root / "probe"),
    ], check=True)

    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    env = os.environ.copy()
    env["RDUID0"] = "1"
    env["RDVT"] = "1"
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core, env=env)
    if result.returncode or "TTY0_STAT_OK" not in result.stdout:
        raise SystemExit(f"tty0 stat guest failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr[-2000:]}")
    print(result.stdout.strip())
