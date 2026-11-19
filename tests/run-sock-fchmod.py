#!/usr/bin/env python3
"""Build and run the socket fchmod/fchown no-op probe."""
import subprocess, tempfile, sys
from pathlib import Path

BASE = Path(__file__).resolve().parent.parent
SHIM = BASE / "shim_cli"

def main():
    with tempfile.TemporaryDirectory(prefix="shimmy-sockfchmod-") as tmp:
        root = Path(tmp)
        (root / "bin").mkdir()
        exe = root / "bin" / "probe"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
            "-fuse-ld=lld", "-Wl,-e,_start", "-O2",
            str(BASE / "tests/guest-sock-fchmod.c"), "-o", str(exe),
        ], check=True, timeout=30)
        r = subprocess.run([str(SHIM), str(root), "/bin/probe"],
                           capture_output=True, text=True, timeout=60)
        out = r.stdout + r.stderr
        if "SOCK_FCHMOD_OK" not in out:
            print(out.strip().splitlines()[-1] if out.strip() else f"(exit {r.returncode})")
            sys.exit(r.returncode or 1)
        print("SOCK_FCHMOD_OK")

if __name__ == "__main__":
    main()
