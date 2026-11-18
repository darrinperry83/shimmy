#!/usr/bin/env python3
"""Verify safe ARM64 sync refusal without invoking host sync."""
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
    repo = Path(__file__).resolve().parent.parent
    sysroot = args.sysroot.resolve(strict=True)
    cli = args.cli.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="shimmy-sync-refusal-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir()
        exe = root / "tmp/guest-sync-refusal"
        subprocess.run(["clang", "--target=aarch64-linux-gnu",
                        "--sysroot=" + str(sysroot), "-fuse-ld=lld",
                        "-static-pie", "-fPIE", "-O2",
                        str(repo / "tests/guest-sync-refusal.c"), "-o", str(exe)],
                       check=True, timeout=30)
        result = subprocess.run([str(cli), str(root), "/tmp/guest-sync-refusal"],
                                env=os.environ.copy(), capture_output=True,
                                text=True, timeout=30, preexec_fn=no_core)
        if result.returncode or "GUEST_SYNC_REFUSAL_OK" not in result.stdout:
            raise RuntimeError(f"guest rc={result.returncode}\n{result.stdout}\n"
                               f"{result.stderr[-1600:]}")
        print("GUEST_SYNC_REFUSAL_OK: ENOSYS without host-wide sync")


if __name__ == "__main__":
    main()
