#!/usr/bin/env python3
"""Negative seccomp gate: show a guest can issue a native Darwin syscall.

This is intentionally a harmless getpid call. It must not be mistaken for a
seccomp implementation test: a positive result demonstrates that the current
in-process guest has a syscall path outside route_syscall().
"""
import argparse
import os
from pathlib import Path
import resource
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sysroot", type=Path, required=True)
    parser.add_argument("--cli", type=Path, default=Path("./shim_cli"))
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    cli = args.cli.resolve(strict=True)
    sysroot = args.sysroot.resolve(strict=True)

    with tempfile.TemporaryDirectory(prefix="shimmy-seccomp-host-svc-") as tmp:
        root = Path(tmp)
        (root / "tmp").mkdir(mode=0o777)
        exe = root / "tmp/guest-host-svc"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "--sysroot=" + str(sysroot),
            "-fuse-ld=lld", "-static-pie", "-fPIE", "-O2",
            str(repo / "tests/guest_seccomp_host_svc.c"), "-o", str(exe),
        ], check=True, timeout=30)
        result = subprocess.run([str(cli), str(root), "/tmp/guest-host-svc"],
                                env=os.environ.copy(), capture_output=True, text=True,
                                timeout=30, preexec_fn=no_core)
        if result.returncode or not any(
                line.startswith("HOST_SVC_RESULT ") and line.split()[-1].isdigit()
                and int(line.split()[-1]) > 0
                for line in result.stdout.splitlines()):
            raise RuntimeError(f"negative gate did not reproduce: rc={result.returncode}\n"
                               f"{result.stdout}\n{result.stderr[-2000:]}")
        print(result.stdout.strip())
        print("SECCOMP_HOST_SVC_BYPASS_CONFIRMED: keep guest filter install disabled")


if __name__ == "__main__":
    main()
