#!/usr/bin/env python3
"""Probe removexattr input safety in a disposable guest root."""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="shimmy-removexattr-input-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir()
        path = root / "tmp/xattr-file"
        path.write_text("fixture")
        subprocess.run(["xattr", "-w", "user.shimmy", "value", str(path)],
                       check=True, timeout=10)
        guest = root / "tmp/guest-removexattr-input"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
            "-fuse-ld=lld", "-Wl,-e,_start",
            str(repo / "tests/guest-removexattr-input.c"), "-o", str(guest),
        ], check=True, timeout=30)
        result = subprocess.run([
            str((repo / "shim_cli").resolve(strict=True)), str(root),
            "/tmp/guest-removexattr-input"], capture_output=True, text=True,
            timeout=30, preexec_fn=no_core)
        if result.returncode or "GUEST_REMOVEXATTR_INPUT_OK" not in result.stdout:
            raise RuntimeError(f"guest failed rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        attrs = subprocess.check_output(["xattr", str(path)], text=True, timeout=10)
        if "user.shimmy" in attrs.splitlines():
            raise RuntimeError("xattr was not removed from disposable file")
        print("GUEST_REMOVEXATTR_INPUT_OK: bad pointers refused; xattr removed")


if __name__ == "__main__":
    main()
