#!/usr/bin/env python3
"""Run an ARM64 pwrite64 guest probe against a disposable file."""
import argparse
import os
from pathlib import Path
import resource
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--sysroot", type=Path, required=True)
args = parser.parse_args()
(repo / "build").mkdir(exist_ok=True)

def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

with tempfile.TemporaryDirectory(prefix="pwrite68-", dir=repo / "build") as tmp:
    root = Path(tmp)
    (root / "work").mkdir()
    (root / "work/file").write_bytes(b"abcdefghijklmnop")
    (root / "bin").mkdir()
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu",
        "--sysroot=" + str(args.sysroot.resolve()), "-fuse-ld=lld",
        "-static-pie", "-fPIE", "-O2",
        str(repo / "tests/guest-pwrite64-checked.c"),
        "-o", str(root / "bin/probe"),
    ], check=True, timeout=30)
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core, env=os.environ.copy())
    if result.returncode or "GUEST_PWRITE64_CHECKED_OK" not in result.stdout:
        raise SystemExit(f"pwrite64 guest failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr[-1600:]}")
    print(result.stdout.strip())
