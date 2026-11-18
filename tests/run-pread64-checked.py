#!/usr/bin/env python3
"""Build and run a disposable ARM64 pread64 output probe."""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="pread64-checked67-") as temp:
    root = Path(temp)
    (root / "tmp").mkdir()
    (root / "bin").mkdir()
    (root / "tmp/data").write_bytes(b"0123456789")
    guest = root / "bin/probe"
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-pread64-checked.c"),
        "-o", str(guest),
    ], check=True, timeout=30)
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core)
    if result.returncode or result.stdout != "PREAD64_CHECKED_OK\n":
        raise RuntimeError(f"pread64 guest failed rc={result.returncode}:\n"
                           f"{result.stdout}\n{result.stderr[-1200:]}")
    print("PREAD64_CHECKED_OK stack/static, partial EOF, offset and invalid outputs")
