#!/usr/bin/env python3
"""Exercise getresuid on disposable native ARM64 guests."""
import os
from pathlib import Path
import resource
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
(repo / "build").mkdir(exist_ok=True)

def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

for virtual in (False, True):
    with tempfile.TemporaryDirectory(prefix="getresuid148-", dir=repo / "build") as tmp:
        root = Path(tmp)
        (root / "bin").mkdir()
        cmd = ["clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
               "-fuse-ld=lld", "-Wl,-e,_start"]
        if virtual:
            cmd.append("-DVIRTUAL_UID=1")
        subprocess.run(cmd + [str(repo / "tests/guest-getresuid.c"),
                              "-o", str(root / "bin/probe")], check=True)
        env = os.environ.copy()
        env.pop("RDUID0", None)
        if virtual:
            env["RDUID0"] = "1"
        result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                                capture_output=True, text=True, timeout=20,
                                preexec_fn=no_core, env=env)
        if result.returncode or "GUEST_GETRESUID_OK" not in result.stdout:
            raise SystemExit(f"getresuid virtual={virtual} failed ({result.returncode}):\n"
                             f"{result.stdout}{result.stderr}")
print("GUEST_GETRESUID_OK virtual and nonvirtual")
