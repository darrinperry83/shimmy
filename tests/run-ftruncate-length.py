#!/usr/bin/env python3
"""Run a disposable ARM64 ftruncate length and seal-order probe."""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="shimmy-ftruncate-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir()
        file = root / "tmp/ftruncate-a"
        file.write_bytes(b"ABCDE")
        guest = root / "tmp/guest-ftruncate-length"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
            "-fuse-ld=lld", "-Wl,-e,_start",
            str(repo / "tests/guest-ftruncate-length.c"), "-o", str(guest),
        ], check=True, timeout=30)
        result = subprocess.run([
            str((repo / "shim_cli").resolve(strict=True)), str(root),
            "/tmp/guest-ftruncate-length"], capture_output=True, text=True,
            timeout=30, preexec_fn=no_core)
        if result.returncode or "GUEST_FTRUNCATE_LENGTH_OK" not in result.stdout:
            raise RuntimeError(f"guest failed rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        if file.read_bytes() != b"ABC\x00\x00\x00\x00":
            raise RuntimeError("ftruncate shorten/extend result is wrong")
        print("GUEST_FTRUNCATE_LENGTH_OK: negative length, sealed fd, file effects")


if __name__ == "__main__":
    main()
