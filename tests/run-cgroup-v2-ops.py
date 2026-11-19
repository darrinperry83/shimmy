#!/usr/bin/env python3
"""Cgroup v2 synthesis gaps: name_to_handle_at + inotify_add_watch + attribute writes."""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="shimmy-cgroup-v2-ops-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir()
        guest = root / "tmp/guest-cgroup-v2-ops"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
            "-fuse-ld=lld", "-Wl,-e,_start",
            str(repo / "tests/guest-cgroup-v2-ops.c"), "-o", str(guest),
        ], check=True, timeout=30)
        result = subprocess.run([
            str((repo / "shim_cli").resolve(strict=True)), str(root),
            "/tmp/guest-cgroup-v2-ops"], capture_output=True, text=True,
            timeout=30, preexec_fn=no_core)
        if result.returncode or "CGROUPFIX_OK" not in result.stdout:
            raise RuntimeError(f"guest failed rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        print("CGROUPFIX_OK: cgroup handle + inotify watch + writable memory attributes")


if __name__ == "__main__":
    main()
