#!/usr/bin/env python3
"""Probe chdir guest path safety and relative cwd in a disposable rootfs."""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="shimmy-chdir-path-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir()
        (root / "a/sub").mkdir(parents=True)
        (root / "a/sub/marker").write_bytes(b"fixture")
        guest = root / "tmp/guest-chdir-path"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
            "-fuse-ld=lld", "-Wl,-e,_start",
            str(repo / "tests/guest-chdir-path.c"), "-o", str(guest),
        ], check=True, timeout=30)
        result = subprocess.run([
            str((repo / "shim_cli").resolve(strict=True)), str(root),
            "/tmp/guest-chdir-path"], capture_output=True, text=True,
            timeout=30, preexec_fn=no_core)
        if result.returncode or "GUEST_CHDIR_PATH_OK" not in result.stdout:
            raise RuntimeError(f"guest failed rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        print("GUEST_CHDIR_PATH_OK: invalid paths refused; cwd and relative open work")


if __name__ == "__main__":
    main()
