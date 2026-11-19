#!/usr/bin/env python3
"""Build and run a disposable ARM64 signal-mask probe for pselect6/ppoll.

Verifies a non-NULL signal mask is honored (not rejected with EOPNOTSUPP):
pselect6/ppoll return 0 on timeout with the mask restored, and a parent-death
signal that becomes pending while masked interrupts pselect6 with EINTR.
"""
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


with tempfile.TemporaryDirectory(prefix="sigmask-", dir=repo / "build") as tmp:
    root = Path(tmp)
    (root / "tmp").mkdir()
    exe = root / "probe"
    env = os.environ.copy()
    env["ZIG_GLOBAL_CACHE_DIR"] = str(root / "zig-cache")
    subprocess.run(["zig", "cc", "-target", "aarch64-linux-musl", "-static-pie",
                    "-fPIE", "-O2", str(repo / "tests/guest-sigmask.c"),
                    "-o", str(exe)], check=True, timeout=60, env=env)
    result = subprocess.run([str(cli), str(root), "/probe"], env=env,
                            capture_output=True, text=True, timeout=40,
                            preexec_fn=no_core)
    out = result.stdout + result.stderr
    if "SIGMASK_GUEST_OK" not in out:
        raise SystemExit(f"sigmask guest failed ({result.returncode}):\n{out}")
    if "SIGMASK_EINTR_OK" not in out:
        raise SystemExit(f"sigmask guest EINTR path failed ({result.returncode}):\n{out}")
    print(result.stdout.strip())
    if result.stderr.strip():
        print(result.stderr.strip())
