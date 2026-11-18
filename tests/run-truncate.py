#!/usr/bin/env python3
"""Build and run the disposable ARM64 truncate guest probe."""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="shimmy-truncate-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir()
        file = root / "tmp/truncate-a"
        file.write_bytes(b"ABCDE")
        guest = root / "tmp/guest-truncate"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
            "-fuse-ld=lld", "-Wl,-e,_start",
            str(repo / "tests/guest-truncate.c"), "-o", str(guest),
        ], check=True, timeout=30)
        result = subprocess.run([
            str((repo / "shim_cli").resolve(strict=True)), str(root),
            "/tmp/guest-truncate"], capture_output=True, text=True,
            timeout=30, preexec_fn=no_core)
        if result.returncode or "GUEST_TRUNCATE_OK" not in result.stdout:
            raise RuntimeError(f"guest failed rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        if file.read_bytes() != b"ABC\x00\x00\x00\x00":
            raise RuntimeError("truncate shorten/extend result is wrong")
        print("GUEST_TRUNCATE_OK: EFAULT, EINVAL, ENAMETOOLONG, shorten/extend")


if __name__ == "__main__":
    main()
