#!/usr/bin/env python3
"""Verify scheduler target identity before and after a disposable guest fork."""
import os
import resource
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
(repo / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="sched-alias-", dir=repo / "build") as tmp:
    root = Path(tmp)
    (root / "bin").mkdir()
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-sched-main-tid-alias.c"),
        "-o", str(root / "bin/probe"),
    ], check=True)
    (root / "sbin").mkdir()
    (root / "sbin/init").symlink_to("../bin/probe")

    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    commands = (
        ([str(repo / "shim_cli"), str(root), "/bin/probe"], "ordinary"),
        (["/bin/zsh", "-c", 'export RDVT_ROOT_PID=$$; exec "$1" "$2" /sbin/init "$$"',
          "zsh", str(repo / "shim_cli"), str(root)], "init"),
    )
    for command, mode in commands:
        result = subprocess.run(command, capture_output=True, text=True, timeout=30,
                                preexec_fn=no_core, env=os.environ.copy())
        if result.returncode or result.stdout.count("GUEST_SCHED_MAIN_TID_ALIAS_OK") != 2:
            raise SystemExit(f"scheduler {mode} identity failed ({result.returncode}):\n"
                             f"{result.stdout}{result.stderr}")
        print(f"GUEST_SCHED_MAIN_TID_ALIAS_OK {mode} parent and forked child")
