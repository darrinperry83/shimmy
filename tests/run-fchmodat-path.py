#!/usr/bin/env python3
"""Probe fchmodat guest pathname safety on a disposable rootfs file."""
from pathlib import Path
import resource
import stat
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="shimmy-fchmodat-path-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir()
        file = root / "tmp/fchmodat-path-file"
        file.write_bytes(b"fixture")
        file.chmod(0o600)
        guest = root / "tmp/guest-fchmodat-path"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
            "-fuse-ld=lld", "-Wl,-e,_start",
            str(repo / "tests/guest-fchmodat-path.c"), "-o", str(guest),
        ], check=True, timeout=30)
        result = subprocess.run([
            str((repo / "shim_cli").resolve(strict=True)), str(root),
            "/tmp/guest-fchmodat-path"], capture_output=True, text=True,
            timeout=30, preexec_fn=no_core)
        if result.returncode or "GUEST_FCHMODAT_PATH_OK" not in result.stdout:
            raise RuntimeError(f"guest failed rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        actual = stat.S_IMODE(file.stat().st_mode)
        if actual != 0o640:
            raise RuntimeError(f"disposable file mode {actual:o}, expected 640")
        print("GUEST_FCHMODAT_PATH_OK: bad guest pointers refused; file chmod works")


if __name__ == "__main__":
    main()
