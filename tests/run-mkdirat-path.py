#!/usr/bin/env python3
"""Probe mkdirat guest path safety in a disposable rootfs."""
from pathlib import Path
import resource
import stat
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="shimmy-mkdirat-path-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir()
        (root / "a").mkdir()
        guest = root / "tmp/guest-mkdirat-path"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
            "-fuse-ld=lld", "-Wl,-e,_start",
            str(repo / "tests/guest-mkdirat-path.c"), "-o", str(guest),
        ], check=True, timeout=30)
        result = subprocess.run([
            str((repo / "shim_cli").resolve(strict=True)), str(root),
            "/tmp/guest-mkdirat-path"], capture_output=True, text=True,
            timeout=30, preexec_fn=no_core)
        if result.returncode or "GUEST_MKDIRAT_PATH_OK" not in result.stdout:
            raise RuntimeError(f"guest failed rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        for name, mode in (("newabs", 0o700), ("newrel", 0o750)):
            path = root / "a" / name
            if not path.is_dir() or stat.S_IMODE(path.stat().st_mode) != mode:
                raise RuntimeError(f"disposable directory {name} missing or wrong mode")
        print("GUEST_MKDIRAT_PATH_OK: bad paths refused; absolute/relative mkdir works")


if __name__ == "__main__":
    main()
