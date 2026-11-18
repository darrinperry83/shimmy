#!/usr/bin/env python3
"""Probe openat guest pathname safety in a disposable rootfs."""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="shimmy-openat-input-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir()
        (root / "tmp/fixture").write_text("Fixture")
        (root / "tmp/fixture-link").symlink_to("fixture")
        guest = root / "tmp/guest-openat-input"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
            "-fuse-ld=lld", "-Wl,-e,_start",
            str(repo / "tests/guest-openat-input.c"), "-o", str(guest),
        ], check=True, timeout=30)
        result = subprocess.run([
            str((repo / "shim_cli").resolve(strict=True)), str(root),
            "/tmp/guest-openat-input"], capture_output=True, text=True,
            timeout=30, preexec_fn=no_core)
        if result.returncode or "GUEST_OPENAT_INPUT_OK" not in result.stdout:
            raise RuntimeError(f"guest failed rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        if not (root / "tmp/created").is_file():
            raise RuntimeError("O_CREAT did not create disposable file")
        print("GUEST_OPENAT_INPUT_OK: bad paths refused; read/create/nofollow work")


if __name__ == "__main__":
    main()
