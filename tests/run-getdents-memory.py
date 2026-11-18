#!/usr/bin/env python3
"""Build and run a disposable ARM64 getdents64 output/cursor probe."""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="getdents-memory61-") as temp:
    root = Path(temp)
    (root / "tmp/testdir").mkdir(parents=True)
    (root / "tmp/testdir/one").write_text("1")
    (root / "tmp/testdir/two").write_text("2")
    (root / "proc").mkdir()
    (root / "bin").mkdir()
    guest = root / "bin/probe"
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-getdents-memory.c"),
        "-o", str(guest),
    ], check=True, timeout=30)
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core)
    if result.returncode or result.stdout != "GETDENTS_MEMORY_OK\n":
        raise RuntimeError(f"getdents guest failed rc={result.returncode}:\n"
                           f"{result.stdout}\n{result.stderr[-1200:]}")
    print("GETDENTS_MEMORY_OK real/synthetic cursors, bad/RX output, record packing")
