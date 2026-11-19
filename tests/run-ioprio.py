#!/usr/bin/env python3
"""Build and run the ioprio_set/ioprio_get guest probe."""
import subprocess, tempfile, os, sys
from pathlib import Path

BASE = Path(__file__).resolve().parent.parent
SHIM = BASE / "shim_cli"

def main():
    with tempfile.TemporaryDirectory(prefix="shimmy-ioprio-") as tmp:
        root = Path(tmp)
        (root / "bin").mkdir()
        exe = root / "bin" / "probe"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
            "-fuse-ld=lld", "-Wl,-e,_start", "-O2",
            str(BASE / "tests/guest-ioprio.c"), "-o", str(exe),
        ], check=True, timeout=30)
        r = subprocess.run([str(SHIM), str(root), "/bin/probe"],
                           capture_output=True, text=True, timeout=60)
        out = r.stdout + r.stderr
        print(out.strip().splitlines()[-1] if out.strip() else f"(exit {r.returncode})")
        if "IOPRIO_GUEST_OK" not in out:
            sys.exit(r.returncode or 1)

if __name__ == "__main__":
    main()
