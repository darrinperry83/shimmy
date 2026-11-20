#!/usr/bin/env python3
"""Regression: mkdir_p must not widen the socket-group root to 0755.

The shim relocates guest AF_UNIX sockets into RDSOCKETGROUP (created 0700 by the
launcher). mkdir_p() used to chmod every path component to 0755, including the
group root, so the first bind() silently widened the group to 0755. pidfd_shim's
socket_group() requires owner-only mode, so every subsequent exec'd process
failed pidfd_shim_target_init() and the shim exited 1 before the guest ran —
which is why a native-session boot showed "Main process exited, status=1" for
every systemd service.

Builds a disposable ARM64 guest that bind()s a socket under /run/sockgroup (thus
forcing mkdir_p on <group>/run/sockgroup) and then execve()s a child. With
RDNATIVE_SESSION=1 the child's shim runs pidfd_shim_target_init(); if the group
was widened the child exits before emitting SOCKGROUP_OK.
"""
import os
import resource
import stat
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
(repo / "build").mkdir(exist_ok=True)

with tempfile.TemporaryDirectory(prefix="sockgroup-", dir=repo / "build") as tmp:
    root = Path(tmp)
    (root / "bin").mkdir()
    (root / "tmp").mkdir()
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-sockgroup.c"),
        "-o", str(root / "bin/probe"),
    ], check=True)

    group = tempfile.mkdtemp(prefix="rd-sockgroup-", dir="/tmp")
    os.chmod(group, 0o700)

    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    env = os.environ.copy()
    env["RDUID0"] = "1"
    env["RDSOCKETGROUP"] = group
    env["RDNATIVE_SESSION"] = "1"
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core, env=env)

    # The group root must still be owner-only after the guest bind().
    mode = stat.S_IMODE(os.stat(group).st_mode)
    if mode & 0o077:
        raise SystemExit(f"sockgroup root was widened to {oct(mode)} "
                         f"(expected owner-only 0o700)")

    if result.returncode or "SOCKGROUP_OK" not in result.stdout:
        raise SystemExit(f"sockgroup failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr[-2000:]}")
    print(result.stdout.strip())
