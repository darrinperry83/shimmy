#!/usr/bin/env python3
"""Run setpgid ARM64 probes as a regular guest and as scoped guest PID 1."""
import os
import resource
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
(repo / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="setpgid154-", dir=repo / "build") as tmp:
    root = Path(tmp)
    (root / "bin").mkdir()
    (root / "sbin").mkdir()
    subprocess.run(["clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
                    "-fuse-ld=lld", "-Wl,-e,_start",
                    str(repo / "tests/guest-setpgid-scope.c"),
                    "-o", str(root / "bin/probe")], check=True)
    (root / "sbin/init").symlink_to("../bin/probe")

    def prepare_child(init):
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        if init:
            # The boot owner and this subprocess are the same host PID.
            os.environ["RDVT_ROOT_PID"] = str(os.getpid())
        else:
            os.environ.pop("RDVT_ROOT_PID", None)

    for guest_path, init in (("/bin/probe", False), ("/sbin/init", True)):
        result = subprocess.run([str(repo / "shim_cli"), str(root), guest_path],
                                capture_output=True, text=True, timeout=20,
                                preexec_fn=lambda: prepare_child(init))
        expected = "GUEST_SETPGID_INIT_OK" if init else "GUEST_SETPGID_REGULAR_OK"
        if result.returncode or expected not in result.stdout:
            raise SystemExit(f"setpgid {guest_path} failed ({result.returncode}):\n"
                             f"{result.stdout}{result.stderr}")
        print(f"{guest_path}: {result.stdout.strip()}")
