#!/usr/bin/env python3
"""Exercise the monotonic no_new_privs fork and exec inheritance in a disposable guest root."""
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
    cli = args.cli.resolve(strict=True)
    sysroot = args.sysroot.resolve(strict=True)

    with tempfile.TemporaryDirectory(prefix="shimmy-nnp-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir(mode=0o777)
        exe = root / "tmp/guest-nnp"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "--sysroot=" + str(sysroot),
            "-fuse-ld=lld", "-static-pie", "-fPIE", "-O2", "-pthread",
            str(repo / "tests/guest_no_new_privs.c"), "-o", str(exe),
        ], check=True, timeout=30)
        result = subprocess.run([str(cli), str(root), "/tmp/guest-nnp"],
                                env=os.environ.copy(), capture_output=True, text=True,
                                timeout=30, preexec_fn=no_core)
        if result.returncode or "GUEST_NNP_FORK_EXEC_OK" not in result.stdout:
            raise RuntimeError(f"guest failed: rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        print("GUEST_NNP_FORK_EXEC_OK: monotonic state, thread, fork and authenticated exec carry")


if __name__ == "__main__":
    main()
