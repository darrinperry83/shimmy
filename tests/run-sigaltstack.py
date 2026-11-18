#!/usr/bin/env python3
"""Build and run a disposable raw ARM64 sigaltstack probe."""
import os
import argparse
import resource
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--sysroot", type=Path)
args = parser.parse_args()
(repo / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="sigaltstack132-", dir=repo / "build") as tmp:
    root = Path(tmp)
    (root / "bin").mkdir()
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-sigaltstack.c"),
        "-o", str(root / "bin/probe"),
    ], check=True)

    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=20,
                            preexec_fn=no_core, env=os.environ.copy())
    if result.returncode or "GUEST_SIGALTSTACK_OK" not in result.stdout:
        raise SystemExit(f"sigaltstack guest failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr}")
    print(result.stdout.strip())
    if args.sysroot:
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu",
            "--sysroot=" + str(args.sysroot.resolve()), "-fuse-ld=lld",
            "-static-pie", "-fPIE", "-O2", "-pthread",
            str(repo / "tests/guest-sigaltstack-threads.c"),
            "-o", str(root / "bin/threads"),
        ], check=True)
        result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/threads"],
                                capture_output=True, text=True, timeout=20,
                                preexec_fn=no_core, env=os.environ.copy())
        if result.returncode or "GUEST_SIGALTSTACK_THREADS_OK" not in result.stdout:
            raise SystemExit(f"sigaltstack thread guest failed ({result.returncode}):\n"
                             f"{result.stdout}{result.stderr}")
        print(result.stdout.strip())
