#!/usr/bin/env python3
"""Exercise bounded ARM64 setfsuid in ordinary and virtual-root guest modes."""
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
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    sysroot = args.sysroot.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="shimmy-setfsuid-nochange-") as tmp:
        root = Path(tmp) / "root"
        (root / "tmp").mkdir(parents=True)
        exe = root / "tmp/guest-setfsuid-nochange"
        subprocess.run([
            "clang", "--target=aarch64-linux-gnu", "--sysroot=" + str(sysroot),
            "-fuse-ld=lld", "-static-pie", "-fPIE", "-O2",
            str(repo / "tests/guest-setfsuid-nochange.c"), "-o", str(exe),
        ], check=True, timeout=30)
        for virtual_root in (False, True):
            env = os.environ.copy()
            if virtual_root:
                env["RDUID0"] = "1"
            else:
                env.pop("RDUID0", None)
            result = subprocess.run([str((repo / "shim_cli").resolve(strict=True)),
                                     str(root), "/tmp/guest-setfsuid-nochange"],
                                    env=env, capture_output=True, text=True,
                                    timeout=30, preexec_fn=no_core)
            if result.returncode or "GUEST_SETFSUID_NOCHANGE_OK" not in result.stdout:
                raise RuntimeError(f"guest failed virtual_root={virtual_root} "
                                   f"rc={result.returncode}\n{result.stdout}\n"
                                   f"{result.stderr[-2000:]}")
        print("GUEST_SETFSUID_NOCHANGE_OK: ordinary and virtual root")


if __name__ == "__main__":
    main()
