#!/usr/bin/env python3
"""Build and run a disposable ARM64 symlinkat guest probe."""
from pathlib import Path
import os
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="shimmy-symlinkat-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir()
        guest = root / "tmp/guest-symlinkat"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
            "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-symlinkat.c"),
            "-o", str(guest),
        ], check=True, timeout=30)
        result = subprocess.run([str((repo / "shim_cli").resolve(strict=True)),
                                 str(root), "/tmp/guest-symlinkat"],
                                env=os.environ.copy(), capture_output=True, text=True,
                                timeout=30, preexec_fn=no_core)
        if result.returncode or "GUEST_SYMLINKAT_OK" not in result.stdout:
            raise RuntimeError(f"guest failed rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        link = root / "tmp/symlink-a"
        if not link.is_symlink() or os.readlink(link) != "../ordinary-target":
            raise RuntimeError("host symlink did not preserve guest target text")
        if (root / "tmp/symlink-too-long").exists():
            raise RuntimeError("invalid target created a host symlink")
        print("GUEST_SYMLINKAT_OK: EFAULT, ENAMETOOLONG, EEXIST, exact target")


if __name__ == "__main__":
    main()
