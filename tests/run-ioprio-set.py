#!/usr/bin/env python3
"""Exercise the truthful ioprio_set reset-self subset in a disposable ARM64 guest."""
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
    parser.add_argument("--cli", type=Path, default=Path("./shim_cli"))
    args = parser.parse_args()
    base = Path(__file__).resolve().parent.parent
    cli = args.cli.resolve(strict=True)
    sysroot = args.sysroot.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="shimmy-ioprio-set-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir(mode=0o777)
        exe = root / "tmp/guest-ioprio-set"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "--sysroot=" + str(sysroot),
            "-fuse-ld=lld", "-static-pie", "-fPIE", "-O2",
            str(base / "tests/guest-ioprio-set.c"), "-o", str(exe),
        ], check=True, timeout=30)
        result = subprocess.run([str(cli), str(root), "/tmp/guest-ioprio-set"],
                                env=os.environ.copy(), capture_output=True, text=True,
                                timeout=40, preexec_fn=no_core)
        if result.returncode or "GUEST_IOPRIO_SET_OK" not in result.stdout:
            raise RuntimeError(f"guest failed: rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        print("GUEST_IOPRIO_SET_OK: default self reset and unsupported/invalid cases")


if __name__ == "__main__":
    main()
