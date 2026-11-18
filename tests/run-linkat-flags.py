#!/usr/bin/env python3
"""Build and run a disposable ARM64 linkat flag probe."""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="shimmy-linkat-flags-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir()
        source = root / "tmp/source"
        source.write_text("source file")
        (root / "tmp/source-sym").symlink_to("source")
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
            "-fuse-ld=lld", "-Wl,-e,_start",
            str(repo / "tests/guest-linkat-flags.c"),
            "-o", str(root / "tmp/linkat-probe"),
        ], check=True, timeout=30)
        result = subprocess.run([
            str((repo / "shim_cli").resolve(strict=True)), str(root),
            "/tmp/linkat-probe",
        ], capture_output=True, text=True, timeout=30, preexec_fn=no_core)
        if result.returncode or "GUEST_LINKAT_FLAGS_OK" not in result.stdout:
            raise RuntimeError(f"guest failed rc={result.returncode}: "
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        if (root / "tmp/bad-link").exists() or (root / "tmp/empty-link").exists():
            raise RuntimeError("invalid or unsupported flag created a link")
        plain = root / "tmp/plain-link"
        followed = root / "tmp/followed-link"
        if not plain.is_symlink() or followed.is_symlink() or \
                followed.stat().st_ino != source.stat().st_ino:
            raise RuntimeError(
                "ordinary/FOLLOW link behavior changed: "
                f"plain_sym={plain.is_symlink()} followed_sym={followed.is_symlink()} "
                f"source_ino={source.stat().st_ino} followed_ino={followed.stat().st_ino}")
        print("GUEST_LINKAT_FLAGS_OK")


if __name__ == "__main__":
    main()
