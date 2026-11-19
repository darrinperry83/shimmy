#!/usr/bin/env python3
"""Verify the GnuPG-style hardlink dotlock sequence in a disposable guest root.

GnuPG 2.4.9's common/dotlock.c takes a lock by link(tname, lockname) and then
stat(tname).st_nlink == 2; a stale lock is detected by read_lockfile + kill(pid,0)
== ESRCH, unlinked, and re-taken.  This probe reproduces that exact sequence in
the guest so a regression in link/unlink/stat-nlink semantics fails loudly.
"""
import os
from pathlib import Path
import resource
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
cli = (repo / "shim_cli").resolve(strict=True)
(repo / "build").mkdir(exist_ok=True)


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


with tempfile.TemporaryDirectory(prefix="dotlock-", dir=repo / "build") as tmp:
    root = Path(tmp)
    (root / "tmp").mkdir()
    exe = root / "probe"
    env = os.environ.copy()
    env["ZIG_GLOBAL_CACHE_DIR"] = str(root / "zig-cache")
    subprocess.run(["zig", "cc", "-target", "aarch64-linux-musl", "-static-pie",
                    "-fPIE", "-O2", str(repo / "tests/guest-dotlock.c"),
                    "-o", str(exe)], check=True, timeout=60, env=env)
    result = subprocess.run([str(cli), str(root), "/probe"], env=env,
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core)
    if result.returncode or "GUEST_DOTLOCK_OK" not in result.stdout:
        raise SystemExit(f"dotlock guest failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr}")
    print(result.stdout.strip())
