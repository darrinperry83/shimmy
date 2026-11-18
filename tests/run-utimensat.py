#!/usr/bin/env python3
"""Build and run a disposable ARM64 utimensat probe, then inspect file times."""
import os
import resource
import subprocess
import tempfile
import time
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
(repo / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="utimensat88-", dir=repo / "build") as tmp:
    root = Path(tmp)
    (root / "bin").mkdir()
    (root / "target").write_text("target")
    (root / "link").symlink_to("target")
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-utimensat.c"),
        "-o", str(root / "bin/probe"),
    ], check=True)

    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    before = time.time()
    target_before = (root / "target").stat().st_mtime_ns
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=20,
                            preexec_fn=no_core, env=os.environ.copy())
    if result.returncode or "GUEST_UTIMENSAT_OK" not in result.stdout:
        raise SystemExit(f"utimensat guest failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr}")
    st = (root / "stamp").stat()
    if st.st_atime_ns // 1_000_000_000 != 1_000_000_000 or not before <= st.st_mtime <= time.time() + 1:
        raise SystemExit(f"utimensat host readback mismatch: atime={st.st_atime}, mtime={st.st_mtime}")
    link = (root / "link").lstat()
    target = (root / "target").stat()
    if int(link.st_mtime) != 1_200_000_000 or target.st_mtime_ns != target_before:
        raise SystemExit(f"AT_SYMLINK_NOFOLLOW mismatch: link={link.st_mtime}, target={target.st_mtime}, before={before}")
    print(result.stdout.strip())
