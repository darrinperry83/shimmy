#!/usr/bin/env python3
"""Run a bounded ARM64 preadv2 ABI probe in a disposable rootfs."""
import argparse
import os
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sysroot", type=Path, required=True)
    ap.add_argument("--cli", type=Path, default=Path("./shim_cli"))
    args = ap.parse_args()
    base = Path(__file__).resolve().parent.parent
    sysroot = args.sysroot.resolve(strict=True)
    cli = args.cli.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="shimmy-preadv2-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir(mode=0o777)
        exe = root / "tmp/guest-preadv2"
        subprocess.run(["clang", "--target=aarch64-linux-gnu",
                        "--sysroot=" + str(sysroot), "-fuse-ld=lld",
                        "-static-pie", "-fPIE", "-O2",
                        str(base / "tests/guest_preadv2.c"), "-o", str(exe)],
                       check=True, timeout=30)
        result = subprocess.run([str(cli), str(root), "/tmp/guest-preadv2"],
                                env=os.environ.copy(), capture_output=True,
                                text=True, timeout=30, preexec_fn=no_core)
        if result.returncode or "GUEST_PREADV2_OK" not in result.stdout:
            raise RuntimeError(f"guest rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        print("GUEST_PREADV2_OK: positioned/current-offset reads, x5 flag rejection, errno")


if __name__ == "__main__":
    main()
