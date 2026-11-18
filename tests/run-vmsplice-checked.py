#!/usr/bin/env python3
"""Exercise checked vmsplice in a disposable ARM64 guest."""
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
    with tempfile.TemporaryDirectory(prefix="shimmy-vmsplice-checked-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir(mode=0o777)
        exe = root / "tmp/guest-vmsplice-checked"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "--sysroot=" + str(sysroot),
            "-fuse-ld=lld", "-static-pie", "-fPIE", "-O2",
            str(base / "tests/guest-vmsplice-checked.c"), "-o", str(exe),
        ], check=True, timeout=30)
        result = subprocess.run([str(cli), str(root), "/tmp/guest-vmsplice-checked"],
                                env=os.environ.copy(), capture_output=True, text=True,
                                timeout=40, preexec_fn=no_core)
        if result.returncode or "GUEST_VMSPLICE_CHECKED_OK" not in result.stdout:
            raise RuntimeError(f"guest failed: rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        print("GUEST_VMSPLICE_CHECKED_OK: pipe writes and bad guest vectors")


if __name__ == "__main__":
    main()
