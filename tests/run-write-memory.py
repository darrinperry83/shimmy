#!/usr/bin/env python3
"""Check #64 write staging from stack, static and executable guest memory."""
from pathlib import Path
import os
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="write-memory64-") as temp:
    root = Path(temp)
    (root / "bin").mkdir()
    guest = root / "bin/probe"
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-write-memory.c"),
        "-o", str(guest),
    ], check=True, timeout=30)
    dump = root / "write-trace.txt"
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
        if result.returncode or result.stdout != "SDJWRITE_MEMORY_OK\n":
            raise RuntimeError(f"write trace={tracing} failed rc={result.returncode}:\n"
                               f"{result.stdout}\n{result.stderr[-1200:]}")
    trace = dump.read_text()
    for char in "SDJ":
        if f'write fd=1 n=1: "{char}"' not in trace:
            raise RuntimeError(f"write trace missing {char}: {trace}")
    print("WRITE_MEMORY_OK stack/static/executable, bad pointers, trace off/on")
