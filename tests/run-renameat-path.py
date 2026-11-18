#!/usr/bin/env python3
"""Probe renameat guest pathname safety using a disposable rootfs."""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="shimmy-renameat-path-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir()
        (root / "a").mkdir()
        (root / "a/source").write_text("source-data")
        (root / "a/relative-source").write_text("relative-data")
        guest = root / "tmp/guest-renameat-path"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
            "-fuse-ld=lld", "-Wl,-e,_start",
            str(repo / "tests/guest-renameat-path.c"), "-o", str(guest),
        ], check=True, timeout=30)
        result = subprocess.run([
            str((repo / "shim_cli").resolve(strict=True)), str(root),
            "/tmp/guest-renameat-path"], capture_output=True, text=True,
            timeout=30, preexec_fn=no_core)
        if result.returncode or "GUEST_RENAMEAT_PATH_OK" not in result.stdout:
            raise RuntimeError(f"guest failed rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        if (root / "a/source").exists() or (root / "a/target").read_text() != "source-data":
            raise RuntimeError("absolute rename result incorrect")
        if (root / "a/relative-source").exists() or (root / "a/relative-target").read_text() != "relative-data":
            raise RuntimeError("relative rename result incorrect")
        print("GUEST_RENAMEAT_PATH_OK: bad paths refused; absolute/relative rename works")


if __name__ == "__main__":
    main()
