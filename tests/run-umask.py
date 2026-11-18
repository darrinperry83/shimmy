#!/usr/bin/env python3
"""Build and run a disposable ARM64 umask probe."""
from pathlib import Path
import resource
import stat
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent

def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

with tempfile.TemporaryDirectory(prefix="shimmy-umask-") as tmp:
    root = Path(tmp)
    (root / "bin").mkdir()
    (root / "tmp").mkdir(mode=0o777)
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start",
        str(repo / "tests/guest-umask.c"), "-o", str(root / "bin/probe"),
    ], check=True)
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=20,
                            preexec_fn=no_core)
    if result.returncode or "GUEST_UMASK_OK" not in result.stdout:
        raise SystemExit(f"umask probe failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr[-1800:]}")
    child_mode = stat.S_IMODE((root / "tmp/child").stat().st_mode)
    parent_mode = stat.S_IMODE((root / "tmp/parent").stat().st_mode)
    if (child_mode, parent_mode) != (0o700, 0o755):
        raise SystemExit(f"unexpected child/parent modes: {child_mode:o}/{parent_mode:o}")
print("GUEST_UMASK_OK child 0700, parent 0755")
