#!/usr/bin/env python3
"""Compile and run a bounded pidfd_send_signal probe in a disposable root."""
import os
import resource
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
(repo / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="pidfd-signal424-", dir=repo / "build") as tmp:
    root = Path(tmp)
    (root / "bin").mkdir()
    compile_env = os.environ.copy()
    compile_env["ZIG_GLOBAL_CACHE_DIR"] = str(root / "zig-cache")
    compile_env["ZIG_LOCAL_CACHE_DIR"] = str(root / "zig-local")
    subprocess.run([
        "zig", "cc", "-target", "aarch64-linux-musl", "-static-pie", "-fPIE", "-O2",
        str(repo / "tests/guest-pidfd-send-signal.c"), "-o", str(root / "bin/probe"),
    ], check=True, env=compile_env)

    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core, env=os.environ.copy())
    if result.returncode or "GUEST_PIDFD_SIGNAL0_VALIDATED_OK" not in result.stdout:
        raise SystemExit(f"pidfd signal guest failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr}")
    print(result.stdout.strip())
