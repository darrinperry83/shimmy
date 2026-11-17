#!/usr/bin/env python3
"""Check one direct GLES draw reaches the local Metal broker in an isolated Arch root.

The current Gallium driver still constructs softpipe contexts. Passing this
probe does not certify that a desktop has zero software-graphics fallback.
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
    parser.add_argument('--no-readback', action='store_true',
                        help='verify the normal path omits diagnostic IOSurface reads')
    args = parser.parse_args()
    base = Path(__file__).resolve().parent.parent
    root = args.root.resolve(strict=True)
    if args.root.is_symlink() or not root.is_relative_to(base / 'build'):
        parser.error('root must be a disposable directory under this checkout/build')
    target = root / 'tmp/guest-rdgpu-triangle'
    env = os.environ.copy()
    for key in ('RDMETAL_LEDGER_MOCK', 'RDMETAL_SAME_IMAGE_METAL_FIXTURE',
                'RDMETAL_SOCKET_BROKER'):
        env.pop(key, None)
    env.update(RDENV_GALLIUM_DRIVER='rdmetal',
               RDENV_LIBGL_DRIVERS_PATH='/usr/lib/dri',
               RDENV_RDMETAL_DIRECT_RESOURCE='1',
               RDENV_RDMETAL_SHIM_OP5_MOCK='1')
    if args.no_readback:
        env.pop('RDMETAL_VERIFY_DRAW_PIXEL', None)
    else:
        env['RDMETAL_VERIFY_DRAW_PIXEL'] = '1'

    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    try:
        subprocess.run([
            'clang', '--target=aarch64-linux-gnu', '--sysroot=' + str(root),
            '-fuse-ld=lld', '-D_GNU_SOURCE', '-fPIE', '-pie',
            str(base / 'tests/guest-rdgpu-triangle.c'), '-o', str(target),
            '-lEGL', '-lGLESv2',
        ], check=True, timeout=30)
        result = subprocess.run([str(args.shim.resolve(strict=True)), str(root),
                                 '/tmp/' + target.name], env=env,
                                capture_output=True, text=True, timeout=30,
                                preexec_fn=no_core)
        print(result.stdout + result.stderr)
        if result.returncode or 'GL_RENDERER=rdmetal Metal (direct)' not in result.stdout or \
                'RDMETAL_ORDINARY_TRIANGLE gl_error=0x0' not in result.stdout or \
                'RDMETAL_FIXTURE_DRAW result=0' not in result.stderr or \
                (args.no_readback and 'RDMETAL_FIXTURE_SAMPLED_PIXEL' in result.stderr) or \
                (not args.no_readback and
                 ('RDMETAL_FIXTURE_SAMPLED_PIXEL bgra=0,0,0,0' in result.stderr or
                  'RDMETAL_FIXTURE_SAMPLED_PIXEL' not in result.stderr)):
            raise RuntimeError('direct GLES Metal draw did not complete')
    finally:
        target.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
