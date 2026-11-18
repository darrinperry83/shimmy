#!/usr/bin/env python3
"""Exercise bounded splice in a disposable ARM64 guest."""
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
    with tempfile.TemporaryDirectory(prefix="shimmy-splice-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir(mode=0o777)
        exe = root / "tmp/guest-splice"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "--sysroot=" + str(sysroot),
            "-fuse-ld=lld", "-static-pie", "-fPIE", "-O2",
            str(base / "tests/guest-splice.c"), "-o", str(exe),
        ], check=True, timeout=30)
        try:
            result = subprocess.run([str(cli), str(root), "/tmp/guest-splice"],
                                    env=os.environ.copy(), capture_output=True, text=True,
                                    timeout=40, preexec_fn=no_core)
        except subprocess.TimeoutExpired as exc:
            raise RuntimeError(f"guest timed out:\n{exc.stderr}") from exc
        if result.returncode or "GUEST_SPLICE_OK" not in result.stdout:
            raise RuntimeError(f"guest failed: rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        print("GUEST_SPLICE_OK: file/pipe directions, offsets and errors")


if __name__ == "__main__":
    main()
