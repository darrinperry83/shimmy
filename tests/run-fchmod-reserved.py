#!/usr/bin/env python3
"""Run an AArch64 fchmod probe against a disposable rootfs and internal fd."""
from pathlib import Path
import resource
import stat
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="shimmy-fchmod-reserved-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir()
        file = root / "tmp/fchmod-file"
        file.write_bytes(b"fixture")
        file.chmod(0o600)
        guest = root / "tmp/guest-fchmod-reserved"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
            "-fuse-ld=lld", "-Wl,-e,_start",
            str(repo / "tests/guest-fchmod-reserved.c"), "-o", str(guest),
        ], check=True, timeout=30)
        result = subprocess.run([
            str((repo / "shim_cli").resolve(strict=True)), str(root),
            "/tmp/guest-fchmod-reserved"], capture_output=True, text=True,
            timeout=30, preexec_fn=no_core)
        if result.returncode or "GUEST_FCHMOD_RESERVED_OK" not in result.stdout:
            raise RuntimeError(f"guest failed rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        actual = stat.S_IMODE(file.stat().st_mode)
        if actual != 0o640:
            raise RuntimeError(f"disposable file mode {actual:o}, expected 640")
        print("GUEST_FCHMOD_RESERVED_OK: disposable file chmod; reserved fd refused")


if __name__ == "__main__":
    main()
