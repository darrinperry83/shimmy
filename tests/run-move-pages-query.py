#!/usr/bin/env python3
"""Compile and execute a disposable native ARM64 move_pages query probe."""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="shimmy-move-pages-") as tmp:
    root = Path(tmp)
    (root / "bin").mkdir()
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start",
        str(repo / "tests/guest-move-pages-query.c"),
        "-o", str(root / "bin/probe"),
    ], check=True, timeout=30)
    result = subprocess.run(
        [str(repo / "shim_cli"), str(root), "/bin/probe"],
        capture_output=True, text=True, timeout=20, preexec_fn=no_core,
    )
    if result.returncode or "GUEST_MOVE_PAGES_QUERY_OK" not in result.stdout:
        raise RuntimeError(
            f"guest failed rc={result.returncode}:\n{result.stdout}\n"
            f"{result.stderr[-3000:]}"
        )
    print(result.stdout.strip())
