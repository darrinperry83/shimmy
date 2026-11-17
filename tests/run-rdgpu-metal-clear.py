#!/usr/bin/env python3
"""Exercise the default Linux syscall -> local Metal broker in a disposable root.

This is a broker clear test, not a Gallium or desktop acceleration test.
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
    shim = args.shim.resolve(strict=True)
    target = root / 'tmp/guest-rdgpu-metal-clear'
    env = os.environ.copy()
    for key in ('RDMETAL_LEDGER_MOCK', 'RDMETAL_SAME_IMAGE_METAL_FIXTURE',
                'RDMETAL_SOCKET_BROKER'):
        env.pop(key, None)

    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    try:
        subprocess.run([
            'clang', '--target=aarch64-linux-gnu', '--sysroot=' + str(root),
            '-fuse-ld=lld', '-fPIE', '-pie',
            str(base / 'tests/guest-rdgpu-metal-clear.c'), '-o', str(target),
        ], check=True, timeout=30)
        result = subprocess.run([str(shim), str(root), '/tmp/' + target.name],
                                env=env, capture_output=True, text=True,
                                timeout=30, preexec_fn=no_core)
        print(result.stdout + result.stderr)
        if result.returncode or 'RDGPU_LOCAL_METAL_CLEAR_OK' not in result.stdout or \
                'RDMETAL_FIXTURE_BROKER op=1 status=0' not in result.stderr:
            raise RuntimeError('local Metal broker clear did not complete')
    finally:
        target.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
