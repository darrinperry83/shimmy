#!/usr/bin/env python3
"""Check #192 refusal in a disposable ARM64 guest; no host IPC calls."""
import os
from pathlib import Path
import resource
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
cli = (repo / "shim_cli").resolve(strict=True)
(repo / "build").mkdir(exist_ok=True)


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


with tempfile.TemporaryDirectory(prefix="semtimedop192-", dir=repo / "build") as tmp:
    root = Path(tmp)
    exe = root / "probe"
    env = os.environ.copy()
    env["ZIG_GLOBAL_CACHE_DIR"] = str(root / "zig-cache")
    subprocess.run(["zig", "cc", "-target", "aarch64-linux-musl", "-static-pie",
                    "-fPIE", "-O2", str(repo / "tests/guest_semtimedop_refused.c"),
                    "-o", str(exe)], check=True, timeout=60, env=env)
    with tempfile.TemporaryDirectory(prefix="semtimedop192-sockets-") as socket_group:
        env["RDSOCKETGROUP"] = socket_group
        result = subprocess.run([str(cli), str(root), "/probe"], env=env,
                                capture_output=True, text=True, timeout=20,
                                preexec_fn=no_core)
    if result.returncode or "SEMTIMEDOP_REFUSED_PASS" not in result.stdout:
        raise SystemExit(f"semtimedop guest failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr}")
    print(result.stdout.strip())
