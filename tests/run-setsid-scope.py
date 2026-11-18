#!/usr/bin/env python3
"""Run setsid as an ordinary guest and as virtual PID 1 in disposable roots."""
import os
import resource
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
(repo / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="setsid157-", dir=repo / "build") as tmp:
    root = Path(tmp)
    (root / "bin").mkdir()
    (root / "sbin").mkdir()
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start",
        str(repo / "tests/guest-setsid-scope.c"), "-o", str(root / "bin/probe"),
    ], check=True)
    (root / "sbin/init").symlink_to("../bin/probe")

    def prepare(init):
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        if init:
            os.environ["RDVT_ROOT_PID"] = str(os.getpid())
        else:
            os.environ.pop("RDVT_ROOT_PID", None)

    for path, init in (("/bin/probe", False), ("/sbin/init", True)):
        result = subprocess.run(
            [str(repo / "shim_cli"), str(root), path], capture_output=True,
            text=True, timeout=20, preexec_fn=lambda: prepare(init),
        )
        expected = "GUEST_SETSID_INIT_OK" if init else "GUEST_SETSID_REGULAR_OK"
        if result.returncode or expected not in result.stdout:
            raise RuntimeError(
                f"setsid {path} failed: rc={result.returncode}\n"
                f"{result.stdout}\n{result.stderr[-1600:]}"
            )
        print(f"{path}: {expected}")
