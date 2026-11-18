#!/usr/bin/env python3
"""Build and execute a real arm64 guest proving rseq's honest fallback."""
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
    base = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="shimmy-rseq-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir(mode=0o777)
        exe = root / "tmp/guest-rseq-fallback"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu",
            "--sysroot=" + str(args.sysroot.resolve()), "-fuse-ld=lld",
            "-static-pie", "-fPIE", "-O2", "-pthread",
            str(base / "tests/guest-rseq-fallback.c"), "-o", str(exe),
        ], check=True, timeout=30)
        result = subprocess.run(
            [str((base / "shim_cli").resolve()), str(root),
             "/tmp/guest-rseq-fallback"],
            env=os.environ.copy(), capture_output=True, text=True,
            timeout=20, preexec_fn=no_core,
        )
        if result.returncode or "GUEST_RSEQ_FALLBACK_OK" not in result.stdout:
            raise RuntimeError(
                f"guest failed: rc={result.returncode}\n{result.stdout}\n"
                f"{result.stderr[-3000:]}"
            )
    print("GUEST_RSEQ_FALLBACK_OK: registration refused, memory intact, "
          "threaded atomic fallback works")


if __name__ == "__main__":
    main()
