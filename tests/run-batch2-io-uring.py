#!/usr/bin/env python3
"""Build and run the no-libc arm64 guest exercising the io_uring family and
process_mrelease (batch-2 syscalls)."""
from pathlib import Path
import os
import resource
import subprocess
import tempfile

base = Path(__file__).resolve().parent.parent
(base / "build").mkdir(exist_ok=True)


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


with tempfile.TemporaryDirectory(prefix="batch2-io-uring-", dir=base / "build") as temp:
    root = Path(temp)
    (root / "bin").mkdir()
    exe = root / "bin" / "probe"
    subprocess.run(
        [
            "clang", "--target=aarch64-linux-gnu", "-nostdlib",
            "-static-pie", "-fuse-ld=lld", "-Wl,-e,_start",
            str(base / "tests/guest-io-uring.c"), "-o", str(exe),
        ],
        check=True, timeout=30,
    )
    result = subprocess.run(
        [str(base / "shim_cli"), str(root), "/bin/probe"],
        capture_output=True, text=True, timeout=30,
        preexec_fn=no_core, env=os.environ.copy(),
    )
    if result.returncode or "IO_URING_OK" not in result.stdout:
        raise RuntimeError(
            f"exit={result.returncode}\n{result.stdout}\n{result.stderr[-1800:]}"
        )
    print("IO_URING_OK")
