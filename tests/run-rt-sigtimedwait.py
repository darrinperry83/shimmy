#!/usr/bin/env python3
"""Build and run a disposable ARM64 rt_sigtimedwait guest probe."""
import os
import resource
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
(repo / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="sigtimedwait137-", dir=repo / "build") as tmp:
    root = Path(tmp)
    (root / "bin").mkdir()
    env = os.environ.copy()
    env["ZIG_GLOBAL_CACHE_DIR"] = str(root / "zig-cache")
    env["ZIG_LOCAL_CACHE_DIR"] = str(root / "zig-local")
    subprocess.run(["zig", "cc", "-target", "aarch64-linux-musl", "-static-pie",
                    "-fPIE", "-O2", str(repo / "tests/guest-rt-sigtimedwait.c"),
                    "-o", str(root / "bin/probe")], check=True, env=env)

    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core, env=os.environ.copy())
    if result.returncode or "GUEST_RT_SIGTIMEDWAIT_OK" not in result.stdout:
        raise SystemExit(f"rt_sigtimedwait guest failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr}")
    print(result.stdout.strip())
