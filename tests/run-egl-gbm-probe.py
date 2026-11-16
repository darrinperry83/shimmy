#!/usr/bin/env python3
"""Compile and run one GBM/EGL/GLES clear in a disposable Arch root under build/.

This checks device discovery, context creation and a clear call, not Metal GPU execution.
The caller must supply an isolated copy of the root; never point at a live rootfs.
"""
import argparse
import os
from pathlib import Path
import re
import resource
import shutil
import signal
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--shim', type=Path, required=True)
    args = parser.parse_args()
    base = Path(__file__).resolve().parent.parent
    root = args.root.resolve(strict=True)
    shim = args.shim.resolve(strict=True)
    if args.root.is_symlink() or not root.is_relative_to(base / 'build'):
        parser.error('root must be a disposable directory beneath this checkout/build')
    if not (root / 'usr/include/EGL/egl.h').is_file():
        parser.error('root lacks EGL development headers')
    target = root / 'tmp/shim-egl-gbm-probe'
    group = Path(tempfile.mkdtemp(prefix='shim-egl-', dir='/tmp'))
    env = os.environ.copy()
    env.update(RDDRM='1', RDUID0='1', RDSOCKETGROUP=str(group), EGL_LOG_LEVEL='debug')

    def preexec():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        os.setsid()

    try:
        subprocess.run([
            'clang', '--target=aarch64-linux-gnu', '--sysroot=' + str(root),
            '-fuse-ld=lld', '-fPIE', '-pie', str(base / 'tests/egl_gbm_probe.c'),
            '-o', str(target), '-lEGL', '-lgbm', '-lGLESv2',
        ], check=True, timeout=30)
        proc = subprocess.Popen([str(shim), str(root), '/tmp/shim-egl-gbm-probe'],
                                env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, preexec_fn=preexec)
        try:
            output, _ = proc.communicate(timeout=15)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            output, _ = proc.communicate()
            raise RuntimeError('EGL probe timed out: ' + output[-1000:])
        for line in output.splitlines():
            if ('pci id for fd' in line or 'MESA-LOADER: dlopen(' in line or
                    line.startswith('EGL ') or line.startswith('GBM ') or
                    line.startswith('GL_RENDERER=')):
                print(line)
        if (proc.returncode or not re.search(r'EGL \d+\.\d+ initialized', output) or
                'GBM GLES2 window clear completed' not in output or
                'MESA-LOADER: failed to retrieve device information' in output):
            raise RuntimeError(f'EGL probe exit {proc.returncode}')
    finally:
        target.unlink(missing_ok=True)
        shutil.rmtree(group)


if __name__ == '__main__':
    main()
