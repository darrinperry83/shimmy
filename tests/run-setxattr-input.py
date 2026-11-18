#!/usr/bin/env python3
"""Probe setxattr guest input safety in a disposable guest root."""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="shimmy-setxattr-input-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir()
        path = root / "tmp/xattr-file"
        path.write_text("fixture")
        guest = root / "tmp/guest-setxattr-input"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
            "-fuse-ld=lld", "-Wl,-e,_start",
            str(repo / "tests/guest-setxattr-input.c"), "-o", str(guest),
        ], check=True, timeout=30)
        result = subprocess.run([
            str((repo / "shim_cli").resolve(strict=True)), str(root),
            "/tmp/guest-setxattr-input"], capture_output=True, text=True,
            timeout=30, preexec_fn=no_core)
        if result.returncode or "GUEST_SETXATTR_INPUT_OK" not in result.stdout:
            raise RuntimeError(f"guest failed rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        value = subprocess.check_output(["xattr", "-p", "user.shimmy", str(path)],
                                        text=True, timeout=10).strip()
        if value != "renewed":
            raise RuntimeError(f"unexpected xattr value {value!r}")
        print("GUEST_SETXATTR_INPUT_OK: bad inputs refused; create/replace work")


if __name__ == "__main__":
    main()
