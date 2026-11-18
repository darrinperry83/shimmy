#!/usr/bin/env python3
"""Build and test getresgid in disposable ordinary and virtual-root guests."""
import os
import resource
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


with tempfile.TemporaryDirectory(prefix="getresgid150-", dir=repo / "build") as tmp:
    root = Path(tmp)
    (root / "bin").mkdir()
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-getresgid.c"),
        "-o", str(root / "bin/probe"),
    ], check=True)
    for virtual in (False, True):
        env = os.environ.copy()
        if virtual:
            env["RDUID0"] = "1"
        else:
            env.pop("RDUID0", None)
        result = subprocess.run(
            [str(repo / "shim_cli"), str(root), "/bin/probe"],
            capture_output=True, text=True, timeout=20, preexec_fn=no_core, env=env,
        )
        if result.returncode or "GUEST_GETRESGID_OK" not in result.stdout:
            raise SystemExit(f"getresgid virtual={virtual} failed ({result.returncode}):\n"
                             f"{result.stdout}{result.stderr[-2000:]}")
    print("GUEST_GETRESGID_OK ordinary and virtual root")
