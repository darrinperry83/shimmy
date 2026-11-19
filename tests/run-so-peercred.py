#!/usr/bin/env python3
"""SO_PEERCRED on a forked AF_UNIX SOCK_STREAM peer must report uid=gid=0 under RDUID0.

A forked child (no exec) never publishes a guest credential record; SO_PEERCRED must
still succeed rather than return ENOPROTOOPT, matching Linux's "every process is root
unless it changed credentials" view under the shim's RDUID0 mapping.
"""
import os
import resource
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
(repo / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="so-peercred-", dir=repo / "build") as tmp:
    root = Path(tmp)
    (root / "tmp").mkdir()
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start", str(repo / "tests/guest-so-peercred.c"),
        "-o", str(root / "probe"),
    ], check=True)

    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    env = os.environ.copy()
    env["RDUID0"] = "1"
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core, env=env)
    if result.returncode or "SO_PEERCRED_OK" not in result.stdout:
        raise SystemExit(f"so-peercred guest failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr[-2000:]}")
    print(result.stdout.strip())
