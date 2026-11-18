#!/usr/bin/env python3
"""Build and run a disposable ARM64 mknodat pathname probe."""
from pathlib import Path
import resource
import stat
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="mknodat-path33-") as temp:
    root = Path(temp)
    (root / "tmp").mkdir()
    guest = root / "probe"
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-mknodat-path.c"),
        "-o", str(guest),
    ], check=True, timeout=30)
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core)
    if result.returncode or result.stdout != "MKNODAT_PATH_OK\n":
        raise RuntimeError(f"mknodat guest failed rc={result.returncode}:\n"
                           f"{result.stdout}\n{result.stderr[-1200:]}")
    if not stat.S_ISREG((root / "tmp/regular").stat().st_mode):
        raise RuntimeError("regular file missing")
    if not stat.S_ISFIFO((root / "tmp/fifo").stat().st_mode):
        raise RuntimeError("FIFO missing")
    if {p.name for p in (root / "tmp").iterdir()} != {"regular", "fifo"}:
        raise RuntimeError("unexpected file created by a rejected pathname")
    print("MKNODAT_PATH_OK checked path, regular file, FIFO")
