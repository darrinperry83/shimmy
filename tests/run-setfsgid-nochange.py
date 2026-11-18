#!/usr/bin/env python3
"""Probe ARM64 setfsgid without changing any host credential."""
from pathlib import Path
import os
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="shimmy-setfsgid-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir()
        guest = root / "tmp/guest-setfsgid-nochange"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
            "-fuse-ld=lld", "-Wl,-e,_start",
            str(repo / "tests/guest-setfsgid-nochange.c"), "-o", str(guest),
        ], check=True, timeout=30)
        env = os.environ.copy()
        env["RDUID0"] = "1"
        result = subprocess.run([
            str((repo / "shim_cli").resolve(strict=True)), str(root),
            "/tmp/guest-setfsgid-nochange"], env=env, capture_output=True,
            text=True, timeout=30, preexec_fn=no_core)
        if result.returncode or "GUEST_SETFSGID_NOCHANGE_OK" not in result.stdout:
            raise RuntimeError(f"guest failed rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        print("GUEST_SETFSGID_NOCHANGE_OK: query, no-op, virtual GID, refusal")


if __name__ == "__main__":
    main()
