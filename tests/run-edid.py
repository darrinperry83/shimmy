#!/usr/bin/env python3
"""Build and run the guest EDID probe against the synthetic /dev/dri/card0.

Compiles tests/guest-edid.c as a freestanding aarch64 Linux static PIE, drops it
into a disposable root, and runs it under shim_cli. The probe opens /dev/dri/card0,
reads the primary connector's EDID blob property, and asserts a 128-byte block with
the standard EDID header. Success is the EDID_OK marker on stdout.
"""
import resource
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


with tempfile.TemporaryDirectory(prefix="shimmy-edid-") as temp:
    root = Path(temp)
    (root / "bin").mkdir()
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", "-O2",
        str(repo / "tests/guest-edid.c"), "-o", str(root / "bin/probe"),
    ], check=True, timeout=30)

    with tempfile.TemporaryDirectory(prefix="rdmeta-edid-", dir="/tmp") as group:
        env = {"RDSOCKETGROUP": group, "RDDRM": "1", "RDDRM_TRACE": "1"}
        result = subprocess.run(
            [str(repo / "shim_cli"), str(root), "/bin/probe"],
            capture_output=True, text=True, timeout=30,
            preexec_fn=no_core, env={**__import__("os").environ, **env},
        )
    if result.returncode or "EDID_OK" not in result.stdout:
        raise SystemExit(
            f"EDID probe failed ({result.returncode}):\n"
            f"{result.stdout}{result.stderr[-1600:]}"
        )
print("EDID_OK")
