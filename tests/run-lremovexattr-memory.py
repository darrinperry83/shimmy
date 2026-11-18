#!/usr/bin/env python3
"""Build and run an ARM64 lremovexattr probe in a disposable guest root."""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="lremovexattr15-") as temp:
    root = Path(temp)
    (root / "tmp").mkdir()
    (root / "bin").mkdir()
    target = root / "tmp/data"
    target.write_bytes(b"x")
    link = root / "tmp/link"
    link.symlink_to("data")
    subprocess.run(["xattr", "-w", "user.shimmy-test", "target", str(target)],
                   check=True, timeout=10)
    subprocess.run(["xattr", "-s", "-w", "user.shimmy-test", "link", str(link)],
                   check=True, timeout=10)
    guest = root / "bin/probe"
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-lremovexattr-memory.c"),
        "-o", str(guest),
    ], check=True, timeout=30)
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core)
    if result.returncode or result.stdout != "LREMOVEXATTR_MEMORY_OK\n":
        raise RuntimeError(f"lremovexattr guest failed rc={result.returncode}:\n"
                           f"{result.stdout}\n{result.stderr[-1200:]}")
    print("LREMOVEXATTR_MEMORY_OK checked strings, symlink nofollow, target retained")
