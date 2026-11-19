#!/usr/bin/env python3
"""Verify a cross-process AF_UNIX listener wakes ppoll/pselect6 in the guest.

The server binds+listens an AF_UNIX socket and blocks in ppoll (syscall 73)
and then pselect6 (syscall 72) on the listener while a forked child connect()s
from a different process. macOS poll()/pselect() miss that pending connection;
the shim must re-check the kernel listen queue and report the fd readable so
the server can accept() instead of timing out (the gpg-agent greeting hang).
"""
import os
from pathlib import Path
import resource
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
cli = (repo / "shim_cli").resolve(strict=True)
(repo / "build").mkdir(exist_ok=True)


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


with tempfile.TemporaryDirectory(prefix="pollaccept-", dir=repo / "build") as tmp:
    root = Path(tmp)
    (root / "tmp").mkdir()
    exe = root / "probe"
    env = os.environ.copy()
    env["ZIG_GLOBAL_CACHE_DIR"] = str(root / "zig-cache")
    subprocess.run(["zig", "cc", "-target", "aarch64-linux-musl", "-static-pie",
                    "-fPIE", "-O2", str(repo / "tests/guest-pollaccept.c"),
                    "-o", str(exe)], check=True, timeout=60, env=env)
    result = subprocess.run([str(cli), str(root), "/probe"], env=env,
                            capture_output=True, text=True, timeout=40,
                            preexec_fn=no_core)
    if result.returncode or "POLLACCEPT_GUEST_OK" not in result.stdout:
        raise SystemExit(f"pollaccept guest failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr}")
    print(result.stdout.strip())
