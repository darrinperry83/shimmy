#!/usr/bin/env python3
"""Check writev staging from stack, static and executable guest memory."""
from pathlib import Path
import os
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="writev-memory66-") as temp:
    root = Path(temp)
    (root / "bin").mkdir()
    guest = root / "bin/probe"
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-writev-memory.c"),
        "-o", str(guest),
    ], check=True, timeout=30)
    dump = root / "writev-trace.txt"
    for tracing in (False, True):
        env = os.environ.copy()
        env.pop("RDWRDUMP", None)
        env.pop("RDWRDUMPFILE", None)
        if tracing:
            env["RDWRDUMP"] = "1"
            env["RDWRDUMPFILE"] = str(dump)
        result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                                capture_output=True, text=True, timeout=30,
                                preexec_fn=no_core, env=env)
        if result.returncode or result.stdout != "SDJWRITEV_MEMORY_OK\n":
            raise RuntimeError(f"writev trace={tracing} failed rc={result.returncode}:\n"
                               f"{result.stdout}\n{result.stderr[-1200:]}")
    if 'writev fd=1 n=3: "SDJ"' not in dump.read_text():
        raise RuntimeError(f"writev trace missing or malformed: {dump.read_text()}")
    print("WRITEV_MEMORY_OK stack/static/JIT, invalid pointers, trace off/on")
