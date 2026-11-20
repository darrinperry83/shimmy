#!/usr/bin/env python3
"""Build and run a disposable ARM64 ENOENT-transparency probe for AF_UNIX paths.

Proves that the PATH-BASED file syscalls on a bound AF_UNIX socket path
(newfstatat, fchownat, fchmodat, readlinkat, unlinkat, and O_PATH openat)
resolve to the shim's relocated socket group, not the rootfs: none of them may
return ENOENT after bind(), while all of them must return ENOENT before bind().

The socket group is exercised through the per-boot auto-group (RDSOCKETGROUP is
deliberately left unset), so the test specifically validates the
net_unix_socket_host_path() routing rather than the older RDSOCKETGROUP lookup.
"""
import os
import resource
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
(repo / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="enoent-transparent-", dir=repo / "build") as tmp:
    root = Path(tmp)
    (root / "bin").mkdir()
    # RDUID0 virtual chown records live under /var/lib; make the parent so the
    # after-bind fchownat can store (rather than fail with ENOENT).
    (root / "var" / "lib").mkdir(parents=True)
    subprocess.run([
        "clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
        "-fuse-ld=lld", "-Wl,-e,_start",
        str(repo / "tests/guest-enoent-transparent.c"),
        "-o", str(root / "bin/probe"),
    ], check=True)

    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    env = os.environ.copy()
    env["RDUID0"] = "1"
    # No RDSOCKETGROUP: force the per-boot auto-group so the file-syscall routing
    # must go through net_unix_socket_host_path(), exactly like a single-command run.
    env.pop("RDSOCKETGROUP", None)
    result = subprocess.run([str(repo / "shim_cli"), str(root), "/bin/probe"],
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core, env=env)
    if result.returncode or "ENOENT_TRANSPARENT_OK" not in result.stdout:
        raise SystemExit(f"enoent-transparent guest failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr[-2000:]}")
    print(result.stdout.strip())
