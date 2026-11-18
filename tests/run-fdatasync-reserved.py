#!/usr/bin/env python3
"""Probe fdatasync on a disposable guest file and shim-owned timerfd."""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="shimmy-fdatasync-reserved-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir()
        (root / "tmp/fdatasync-file").write_bytes(b"disposable data")
        guest = root / "tmp/guest-fdatasync-reserved"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
            "-fuse-ld=lld", "-Wl,-e,_start",
            str(repo / "tests/guest-fdatasync-reserved.c"), "-o", str(guest),
        ], check=True, timeout=30)
        result = subprocess.run([
            str((repo / "shim_cli").resolve(strict=True)), str(root),
            "/tmp/guest-fdatasync-reserved"], capture_output=True, text=True,
            timeout=30, preexec_fn=no_core)
        if result.returncode or "GUEST_FDATASYNC_RESERVED_OK" not in result.stdout:
            raise RuntimeError(f"guest failed rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        print("GUEST_FDATASYNC_RESERVED_OK: disposable file sync; internal fd refused")


if __name__ == "__main__":
    main()
