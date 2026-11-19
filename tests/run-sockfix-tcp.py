#!/usr/bin/env python3
"""Regression: a guest TCP listen socket must not leak or steal a host port.

Holds host 127.0.0.1:9000, then runs the guest twice. Both guest binds must
succeed because the shim relocates guest listen ports into a per-boot port space
and releases the mapped host port when the guest process exits.
"""
import os
import resource
import socket
import subprocess
import tempfile
from pathlib import Path

PORT = 9000
repo = Path(__file__).resolve().parent.parent
(repo / "build").mkdir(exist_ok=True)

with tempfile.TemporaryDirectory(prefix="sockfix-tcp-", dir=repo / "build") as tmp:
    root = Path(tmp)
    (root / "bin").mkdir()
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-sockfix-tcp.c"),
        "-o", str(root / "bin/probe"),
    ], check=True)

    # Hold the real host port the guest asks for. The guest must not take it.
    holder = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    holder.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    holder.bind(("127.0.0.1", PORT))
    holder.listen(1)

    group = tempfile.mkdtemp(prefix="rd-sockfix-tcp-", dir="/tmp")

    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    env = os.environ.copy()
    env["RDUID0"] = "1"
    env["RDSOCKETGROUP"] = group

    for attempt in ("first", "second"):
        result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                                capture_output=True, text=True, timeout=30,
                                preexec_fn=no_core, env=env)
        if result.returncode or "SOCKFIX_TCP_BOUND" not in result.stdout:
            raise SystemExit(f"sockfix-tcp {attempt} bind failed "
                             f"({result.returncode}):\n{result.stdout}"
                             f"{result.stderr[-2000:]}")

    # The host's own port must still be ours (the guest never stole it).
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        probe.bind(("127.0.0.1", PORT))
        stolen = False
    except OSError:
        stolen = True
    probe.close()
    holder.close()
    if not stolen:
        raise SystemExit("sockfix-tcp: host port was released — guest stole it")

    print("SOCKFIX_OK")
