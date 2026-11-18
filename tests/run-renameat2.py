#!/usr/bin/env python3
"""Build and run an ARM64 renameat2 probe in a disposable guest root."""
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="shimmy-renameat2-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir()
        (root / "tmp/a").write_text("A")
        (root / "tmp/b").write_text("B")
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
            "-fuse-ld=lld", "-Wl,-e,_start",
            str(repo / "tests/guest-renameat2.c"), "-o", str(root / "tmp/probe"),
        ], check=True, timeout=30)
        result = subprocess.run([str((repo / "shim_cli").resolve(strict=True)),
                                 str(root), "/tmp/probe"],
                                capture_output=True, text=True, timeout=30,
                                preexec_fn=no_core)
        if result.returncode or "GUEST_RENAMEAT2_OK" not in result.stdout:
            raise RuntimeError(f"guest failed rc={result.returncode}: "
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        if (root / "tmp/a").exists() or (root / "tmp/b").read_text() != "A" or \
                (root / "tmp/c").read_text() != "B" or (root / "tmp/missing").exists():
            raise RuntimeError("rename result did not match the two atomic operations")
        print("GUEST_RENAMEAT2_OK")


if __name__ == "__main__":
    main()
