#!/usr/bin/env python3
"""Build and run a disposable ARM64 fchdir/guest-cwd probe."""
import os
import resource
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
(repo / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="fchdir50-", dir=repo / "build") as tmp:
    root = Path(tmp) / "root"
    root.mkdir()
    outside = Path(tmp) / "outside"
    outside.mkdir()
    outside_fd = os.open(outside, os.O_RDONLY)
    (root / "bin").mkdir()
    (root / "a").mkdir()
    (root / "b").mkdir()
    (root / "a/marker").write_text("guest directory marker\n")
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", f"-DOUTSIDE_FD={outside_fd}",
        str(repo / "tests/guest-fchdir.c"),
        "-o", str(root / "bin/probe"),
    ], check=True)

    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    env = os.environ.copy()
    env["RDSHARE"] = ""
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=20,
                            preexec_fn=no_core, pass_fds=(outside_fd,), env=env)
    os.close(outside_fd)
    if result.returncode or "GUEST_FCHDIR_OK" not in result.stdout:
        raise SystemExit(f"fchdir guest failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr}")
    print(result.stdout.strip())
