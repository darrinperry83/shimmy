#!/usr/bin/env python3
"""Exercise bounded sync_file_range in a disposable ARM64 guest."""
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
    with tempfile.TemporaryDirectory(prefix="shimmy-sync-file-range-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir(mode=0o777)
        exe = root / "tmp/guest-sync-file-range"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "--sysroot=" + str(sysroot),
            "-fuse-ld=lld", "-static-pie", "-fPIE", "-O2",
            str(base / "tests/guest-sync-file-range.c"), "-o", str(exe),
        ], check=True, timeout=30)
        result = subprocess.run([str(cli), str(root), "/tmp/guest-sync-file-range"],
                                env=os.environ.copy(), capture_output=True, text=True,
                                timeout=40, preexec_fn=no_core)
        if result.returncode or "GUEST_SYNC_FILE_RANGE_OK" not in result.stdout:
            raise RuntimeError(f"guest failed: rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        print("GUEST_SYNC_FILE_RANGE_OK: zero flags, bad ranges, unsupported writes")


if __name__ == "__main__":
    main()
