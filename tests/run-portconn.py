#!/usr/bin/env python3
"""Regression: full TCP loopback through the shim's per-boot port offset.

Holds host 127.0.0.1:PORT, then runs the guest once. Inside the guest the parent
binds 127.0.0.1:PORT and a forked child connects back to 127.0.0.1:PORT; data flows
both ways and the server's getsockname() / client's getpeername() must report the
ORIGINAL PORT (not the host's offset port). The guest bind must succeed even though
the host still holds PORT (host-port isolation), and the host port must remain ours.
"""
import os
import resource
import socket
import subprocess
import tempfile
from pathlib import Path

PORT = 2222
# Deterministic boot token: FNV-1a("portconn") -> g_port_offset 0x837C, so
# PORT + offset (0x8C2A) stays inside the shim's mapped band 0x8000..0xBFFF.
BOOT = "portconn"

repo = Path(__file__).resolve().parent.parent
(repo / "build").mkdir(exist_ok=True)

with tempfile.TemporaryDirectory(prefix="portconn-", dir=repo / "build") as tmp:
    root = Path(tmp)
    (root / "bin").mkdir()
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-portconn.c"),
        "-o", str(root / "bin/probe"),
    ], check=True)

    # Hold the real host port the guest asks for. The guest must not take it.
    holder = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    holder.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    holder.bind(("127.0.0.1", PORT))
    holder.listen(1)

    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    env = os.environ.copy()
    env["RDUID0"] = "1"
    env["RDSHIM_BOOT"] = BOOT

    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core, env=env)
    if result.returncode or "PORTCONN_OK" not in result.stdout:
        raise SystemExit(f"portconn guest failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr[-2000:]}")

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
        raise SystemExit("portconn: host port was released — guest stole it")

    print("PORTCONN_OK")
