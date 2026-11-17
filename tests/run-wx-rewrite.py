#!/usr/bin/env python3
"""Verify a guest RWX JIT page retains code across execute/write/execute flips.

Run only with a disposable Arch root under this checkout's build directory.
"""
import argparse
import os
from pathlib import Path
import resource
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True, type=Path)
    parser.add_argument('--shim', required=True, type=Path)
    args = parser.parse_args()
    base = Path(__file__).resolve().parent.parent
    root = args.root.resolve(strict=True)
    if args.root.is_symlink() or not root.is_relative_to(base / 'build'):
        parser.error('root must be a disposable directory under this checkout/build')
    target = root / 'tmp/guest-wx-rewrite'
    target.parent.mkdir(exist_ok=True)

    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    try:
        subprocess.run([
            'clang', '--target=aarch64-linux-gnu', '--sysroot=' + str(root),
            '-fuse-ld=lld', '-fPIE', '-pie',
            str(base / 'tests/guest-wx-rewrite.c'), '-o', str(target),
        ], check=True, timeout=30)
        result = subprocess.run([str(args.shim.resolve(strict=True)), str(root),
                                 '/tmp/' + target.name], env=os.environ.copy(),
                                capture_output=True, text=True, timeout=30,
                                preexec_fn=no_core)
        print(result.stdout + result.stderr)
        if result.returncode or 'WX_REWRITE first=7 second=9 ret=d65f03c0' not in result.stdout:
            raise RuntimeError('guest JIT code was lost across a W^X flip')
    finally:
        target.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
