#!/usr/bin/env python3
"""Build and run a disposable ARM64 unlinkat pathname probe."""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="unlinkat-path35-") as temp:
    root = Path(temp)
    (root / "tmp").mkdir()
    (root / "tmp/remove-file").write_bytes(b"x")
    (root / "tmp/keep").write_bytes(b"keep")
    (root / "tmp/remove-dir").mkdir()
    guest = root / "probe"
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-unlinkat-path.c"),
        "-o", str(guest),
    ], check=True, timeout=30)
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core)
    if result.returncode or result.stdout != "UNLINKAT_PATH_OK\n":
        raise RuntimeError(f"unlinkat guest failed rc={result.returncode}:\n"
                           f"{result.stdout}\n{result.stderr[-1200:]}")
    if (root / "tmp/remove-file").exists() or (root / "tmp/remove-dir").exists():
        raise RuntimeError("named removals did not complete")
    if (root / "tmp/keep").read_bytes() != b"keep":
        raise RuntimeError("unrelated file changed")
    print("UNLINKAT_PATH_OK checked path, file/rmdir, unrelated file preserved")
