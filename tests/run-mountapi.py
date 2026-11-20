#!/usr/bin/env python3
"""Probe the new mount API (fsopen/fsconfig/fsmount/move_mount) honesty.

A tmpfs whose destination is /tmp must fail with EPERM, while a tmpfs onto
/run/user/<uid> materializes the runtime directory, and synthetic API
filesystems (proc, /dev/shm, ...) keep their no-op success.
"""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="shimmy-mountapi-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir()
        (root / "run" / "user").mkdir(parents=True)
        (root / "proc").mkdir()
        (root / "dev" / "shm").mkdir(parents=True)
        guest = root / "tmp/guest-mountapi"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
            "-fuse-ld=lld", "-Wl,-e,_start",
            str(repo / "tests/guest-mountapi.c"), "-o", str(guest),
        ], check=True, timeout=30)
        result = subprocess.run([
            str((repo / "shim_cli").resolve(strict=True)), str(root),
            "/tmp/guest-mountapi"], capture_output=True, text=True,
            timeout=30, preexec_fn=no_core)
        if result.returncode or "GUEST_MOUNTAPI_OK" not in result.stdout:
            raise RuntimeError(f"guest failed rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        print("GUEST_MOUNTAPI_OK: tmpfs /tmp EPERM, /run/user/<uid> materialized, proc/shm no-op")


if __name__ == "__main__":
    main()
