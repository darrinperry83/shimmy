#!/usr/bin/env python3
"""Probe ARM64 msgrcv refusal without creating or accessing host IPC."""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="shimmy-msgrcv-refuse-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir()
        guest = root / "tmp/guest-msgrcv-refuse"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
            "-fuse-ld=lld", "-Wl,-e,_start",
            str(repo / "tests/guest-msgrcv-refuse.c"), "-o", str(guest),
        ], check=True, timeout=30)
        result = subprocess.run([
            str((repo / "shim_cli").resolve(strict=True)), str(root),
            "/tmp/guest-msgrcv-refuse"], capture_output=True, text=True,
            timeout=30, preexec_fn=no_core)
        if result.returncode or "GUEST_MSGRCV_REFUSE_OK" not in result.stdout:
            raise RuntimeError(f"guest failed rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        print("GUEST_MSGRCV_REFUSE_OK: host IDs blocked, buffer unchanged")


if __name__ == "__main__":
    main()
