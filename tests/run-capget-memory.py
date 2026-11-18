#!/usr/bin/env python3
"""Exercise ARM64 capget guest memory in a disposable root."""
import argparse
import os
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sysroot", type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    sysroot = args.sysroot.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="shimmy-capget-memory-") as tmp:
        root = Path(tmp) / "root"
        (root / "tmp").mkdir(parents=True)
        exe = root / "tmp/guest-capget-memory"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "--sysroot=" + str(sysroot),
            "-fuse-ld=lld", "-static-pie", "-fPIE", "-O2",
            str(repo / "tests/guest-capget-memory.c"), "-o", str(exe),
        ], check=True, timeout=30)
        result = subprocess.run([str((repo / "shim_cli").resolve(strict=True)),
                                 str(root), "/tmp/guest-capget-memory"],
                                env=os.environ.copy(), capture_output=True,
                                text=True, timeout=30, preexec_fn=no_core)
        if result.returncode or "GUEST_CAPGET_MEMORY_OK" not in result.stdout:
            raise RuntimeError(f"guest failed rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        print("GUEST_CAPGET_MEMORY_OK")


if __name__ == "__main__":
    main()
