#!/usr/bin/env python3
"""Check virtual hostname isolation and fork/exec visibility with disposable ARM64 guests."""
import os
import resource
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
(repo / "build").mkdir(exist_ok=True)

def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

host_before = subprocess.check_output(["hostname"], text=True)
with tempfile.TemporaryDirectory(prefix="sethostname161-", dir=repo / "build") as tmp:
    root = Path(tmp) / "root"
    group = Path(tmp) / "private-group"
    (root / "bin").mkdir(parents=True)
    group.mkdir(mode=0o700)
    for name, define in [("no-group", "NO_GROUP"), ("non-root", "NON_ROOT"),
                         ("shared", "SHARED"),
                         ("verify", "VERIFY_EXEC")]:
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
            "-fuse-ld=lld", "-Wl,-e,_start", f"-D{define}",
            str(repo / "tests/guest-sethostname.c"), "-o", str(root / "bin" / name),
        ], check=True)
    base_env = os.environ.copy()
    base_env.pop("RDSOCKETGROUP", None)
    base_env.pop("RDX_MEMFD_DIR", None)
    base_env["RDUID0"] = "1"
    for program, with_group in [("no-group", False), ("non-root", True),
                                ("shared", True)]:
        env = dict(base_env)
        if with_group:
            env["RDSOCKETGROUP"] = str(group)
        result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/" + program],
                                capture_output=True, text=True, timeout=20,
                                preexec_fn=no_core, env=env)
        if result.returncode or "GUEST_SETHOSTNAME_OK" not in result.stdout:
            raise SystemExit(f"{program} failed ({result.returncode}):\n"
                             f"{result.stdout}{result.stderr}")
host_after = subprocess.check_output(["hostname"], text=True)
if host_before != host_after:
    raise SystemExit("host hostname changed")
print("GUEST_SETHOSTNAME_OK host unchanged")
