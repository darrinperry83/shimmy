#!/usr/bin/env python3
"""Build and run a disposable ARM64 SCM_CREDENTIALS probe (sd_notify shape).

Reproduces systemd's /run/systemd/notify datagram pattern: a bound-but-unconnected
AF_UNIX SOCK_DGRAM receiver with SO_PASSCRED, and a forked sender that sendmsg()s a
datagram without connect(). The receiver must recover an SCM_CREDENTIALS cmsg whose
pid is the sender's pid — proving the shim's source-path credential synthesis works.
"""
import os
import resource
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
(repo / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="sdnotify-scm-", dir=repo / "build") as tmp:
    root = Path(tmp)
    (root / "bin").mkdir()
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-sdnotify-scm.c"),
        "-o", str(root / "bin/probe"),
    ], check=True)

    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    env = os.environ.copy()
    env["RDUID0"] = "1"
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core, env=env)
    if result.returncode or "SDNOTIFY_SCM_OK" not in result.stdout:
        raise SystemExit(f"sdnotify-scm guest failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr[-2000:]}")
    print(result.stdout.strip())
