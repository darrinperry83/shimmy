#!/usr/bin/env python3
"""Check epoll_create1 flag validation in a disposable AArch64 guest."""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="epoll-create-flags20-") as temp:
    root = Path(temp)
    (root / "bin").mkdir()
    guest = root / "bin/probe"
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start",
        str(repo / "tests/guest-epoll-create-flags.c"), "-o", str(guest),
    ], check=True, timeout=30)
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core)
    if result.returncode or result.stdout != "EPOLL_CREATE_FLAGS_OK\n":
        raise RuntimeError(f"epoll_create1 probe failed rc={result.returncode}:\n"
                           f"{result.stdout}\n{result.stderr[-1200:]}")
    print("EPOLL_CREATE_FLAGS_OK invalid flags, plain and CLOEXEC")
