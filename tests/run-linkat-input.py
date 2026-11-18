#!/usr/bin/env python3
"""Probe linkat guest pathname safety in a disposable rootfs."""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="shimmy-linkat-input-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir()
        (root / "a").mkdir()
        source = root / "a/source"
        source.write_text("source-data")
        guest = root / "tmp/guest-linkat-input"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
            "-fuse-ld=lld", "-Wl,-e,_start",
            str(repo / "tests/guest-linkat-input.c"), "-o", str(guest),
        ], check=True, timeout=30)
        result = subprocess.run([
            str((repo / "shim_cli").resolve(strict=True)), str(root),
            "/tmp/guest-linkat-input"], capture_output=True, text=True,
            timeout=30, preexec_fn=no_core)
        if result.returncode or "GUEST_LINKAT_INPUT_OK" not in result.stdout:
            raise RuntimeError(f"guest failed rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        inode = source.stat().st_ino
        for name in ("linked", "relative-linked"):
            target = root / "a" / name
            if target.read_text() != "source-data" or target.stat().st_ino != inode:
                raise RuntimeError(f"{name} is not a hard link to source")
        print("GUEST_LINKAT_INPUT_OK: bad paths refused; absolute/relative hard links work")


if __name__ == "__main__":
    main()
