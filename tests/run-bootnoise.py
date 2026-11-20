#!/usr/bin/env python3
"""Run disposable no-libc ARM64 probe for the three boot-noise fixes."""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="shimmy-bootnoise-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir()
        guest = root / "tmp/guest-bootnoise"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
            "-fuse-ld=lld", "-Wl,-e,_start",
            str(repo / "tests/guest-bootnoise.c"), "-o", str(guest),
        ], check=True, timeout=30)
        result = subprocess.run([
            str((repo / "shim_cli").resolve(strict=True)), str(root),
            "/tmp/guest-bootnoise"], capture_output=True, text=True,
            timeout=30, preexec_fn=no_core)
        if result.returncode or "BOOTNOISE_OK" not in result.stdout:
            raise RuntimeError(f"guest failed rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        print("BOOTNOISE_OK: cpu.idle write + IPv6 recv options + inert inotify watches")


if __name__ == "__main__":
    main()
