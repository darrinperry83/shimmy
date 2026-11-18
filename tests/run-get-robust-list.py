#!/usr/bin/env python3
"""Run the robust-list syscall 100 ABI proof in a disposable guest root."""
import os
import resource
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
(repo / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="robust100-", dir=repo / "build") as tmp:
    root = Path(tmp)
    (root / "bin").mkdir()

    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    for name, marker in (("get-robust-list", "get_robust_list guest ABI PASS"),
                         ("robust-thread", "get_robust_list guest thread PASS")):
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
            "-fuse-ld=lld", "-Wl,-e,_start", str(repo / f"tests/guest-{name}.c"),
            "-o", str(root / "bin/probe"),
        ], check=True)
        result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                                capture_output=True, text=True, timeout=30,
                                preexec_fn=no_core, env=os.environ.copy())
        if result.returncode or marker not in result.stdout:
            raise SystemExit(f"{name} guest failed ({result.returncode}):\n"
                             f"{result.stdout}{result.stderr}")
        print(result.stdout.strip())
