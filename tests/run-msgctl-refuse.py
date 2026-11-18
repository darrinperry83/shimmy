#!/usr/bin/env python3
"""Verify the msgctl host-ID safeguard in a disposable AArch64 guest."""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="shimmy-msgctl-refuse-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir()
        guest = root / "tmp/guest-msgctl-refuse"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
            "-fuse-ld=lld", "-Wl,-e,_start",
            str(repo / "tests/guest-msgctl-refuse.c"), "-o", str(guest),
        ], check=True, timeout=30)
        result = subprocess.run([
            str((repo / "shim_cli").resolve(strict=True)), str(root),
            "/tmp/guest-msgctl-refuse"], capture_output=True, text=True,
            timeout=30, preexec_fn=no_core)
        if result.returncode or "GUEST_MSGCTL_REFUSE_OK" not in result.stdout:
            raise RuntimeError(f"guest failed rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        print("GUEST_MSGCTL_REFUSE_OK: RMID, SET, STAT, unknown command")


if __name__ == "__main__":
    main()
