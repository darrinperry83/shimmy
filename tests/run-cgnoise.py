#!/usr/bin/env python3
"""Regression test for the two cgroup-v2 synthesis gaps systemd hits at boot:
write() to a child cgroup's memory.zswap.max returning EINVAL, and
name_to_handle_at() on a child cgroup returning ENOENT."""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="shimmy-cgnoise-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir()
        guest = root / "tmp/guest-cgnoise"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
            "-fuse-ld=lld", "-Wl,-e,_start",
            str(repo / "tests/guest-cgnoise.c"), "-o", str(guest),
        ], check=True, timeout=30)
        result = subprocess.run([
            str((repo / "shim_cli").resolve(strict=True)), str(root),
            "/tmp/guest-cgnoise"], capture_output=True, text=True,
            timeout=30, preexec_fn=no_core)
        if result.returncode or "CGNOISE_OK" not in result.stdout:
            raise RuntimeError(f"guest failed rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        print("CGNOISE_OK: child-cgroup write reflects read-back + valid cgroup handle")


if __name__ == "__main__":
    main()
