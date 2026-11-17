#!/usr/bin/env python3
"""Exercise guest execveat in a disposable Arch clone; never modify the live root."""
import argparse
import os
import resource
import shutil
import subprocess
from pathlib import Path


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--cli", type=Path, default=Path("./shim_cli"))
    args = parser.parse_args()
    base = Path(__file__).resolve().parent.parent
    root = args.root.resolve(strict=True)
    if args.root.is_symlink() or not root.is_relative_to(base / "build"):
        parser.error("root must be a disposable directory under this checkout/build")
    cli = args.cli.resolve(strict=True)
    target = root / "tmp/guest-execveat"
    link = root / "tmp/execveat-echo-link"
    detached = root / "tmp/execveat-detached"
    target.parent.mkdir(exist_ok=True)
    try:
        subprocess.run(["clang", "--target=aarch64-linux-gnu", "--sysroot=" + str(root),
                        "-fuse-ld=lld", "-fPIE", "-pie",
                        str(base / "tests/guest-execveat.c"), "-o", str(target)], check=True,
                       timeout=30)
        link.symlink_to("/usr/bin/echo")
        shutil.copy2(root / "usr/bin/echo", detached)
        for mode in ("relative", "empty", "nofollow", "bad-flags", "bad-fd",
                     "empty-no-flag", "detached"):
            result = subprocess.run([str(cli), str(root), "/tmp/guest-execveat", mode],
                                    env={**os.environ, "RDUID0": "1"}, capture_output=True,
                                    text=True, timeout=30, preexec_fn=no_core)
            expected = "EXECVEAT_OK" if mode in ("relative", "empty") else "EXECVEAT_ERR_OK"
            if result.returncode or expected not in result.stdout:
                raise RuntimeError(f"{mode}: rc={result.returncode}\n{result.stdout}\n"
                                   f"{result.stderr[-1800:]}")
            print(mode, "passed")
    finally:
        link.unlink(missing_ok=True)
        detached.unlink(missing_ok=True)
        target.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
