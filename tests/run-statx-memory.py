#!/usr/bin/env python3
"""Build and run a disposable ARM64 statx guest-memory probe."""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="statx-memory291-") as temp:
    root = Path(temp)
    (root / "tmp").mkdir()
    (root / "bin").mkdir()
    (root / "tmp/data").write_bytes(b"abc")
    guest = root / "bin/probe"
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-statx-memory.c"),
        "-o", str(guest),
    ], check=True, timeout=30)
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core)
    if result.returncode or result.stdout != "STATX_MEMORY_OK\n":
        raise RuntimeError(f"statx guest failed rc={result.returncode}:\n"
                           f"{result.stdout}\n{result.stderr[-1200:]}")
    print("STATX_MEMORY_OK ordinary, synthetic, AT_EMPTY_PATH, bad/RX pointers")
