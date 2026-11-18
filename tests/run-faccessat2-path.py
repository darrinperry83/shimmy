#!/usr/bin/env python3
"""Build and run a disposable ARM64 faccessat2 pathname probe."""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="faccessat2-path439-") as temp:
    root = Path(temp)
    (root / "tmp").mkdir()
    (root / "tmp/file").write_bytes(b"x")
    guest = root / "probe"
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-faccessat2-path.c"),
        "-o", str(guest),
    ], check=True, timeout=30)
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core)
    if result.returncode or result.stdout != "FACCESSAT2_PATH_OK\n":
        raise RuntimeError(f"faccessat2 guest failed rc={result.returncode}:\n"
                           f"{result.stdout}\n{result.stderr[-1200:]}")
    print("FACCESSAT2_PATH_OK bad/static paths, fd empty path")
