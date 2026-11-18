#!/usr/bin/env python3
"""Probe faccessat guest path safety against a disposable rootfs."""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="shimmy-faccessat-path-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir()
        (root / "a").mkdir()
        (root / "a/marker").write_bytes(b"fixture")
        guest = root / "tmp/guest-faccessat-path"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
            "-fuse-ld=lld", "-Wl,-e,_start",
            str(repo / "tests/guest-faccessat-path.c"), "-o", str(guest),
        ], check=True, timeout=30)
        result = subprocess.run([
            str((repo / "shim_cli").resolve(strict=True)), str(root),
            "/tmp/guest-faccessat-path"], capture_output=True, text=True,
            timeout=30, preexec_fn=no_core)
        if result.returncode or "GUEST_FACCESSAT_PATH_OK" not in result.stdout:
            raise RuntimeError(f"guest failed rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        print("GUEST_FACCESSAT_PATH_OK: bad paths refused; file and dirfd access work")


if __name__ == "__main__":
    main()
