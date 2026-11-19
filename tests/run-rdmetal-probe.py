#!/usr/bin/env python3
"""Compile and run the virtual DRM node probe in a disposable Arch root.

This opens /dev/dri/card0 and /dev/dri/renderD128, reads the DRM capabilities,
enumerates CRTCs/connectors/encoders/planes/properties, creates+mmaps a dumb
buffer, adds an fb2, and exports a PRIME fd. It asserts every step and emits
RDMETAL_PROBE_OK on success. The caller must supply an isolated copy of the
root; never point at a live rootfs.
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
    if not root.is_relative_to(base / 'build'):
        parser.error('root must be a disposable directory beneath this checkout/build')
    for header in ('usr/include/xf86drm.h', 'usr/include/libdrm/drm.h',
                   'usr/lib/libdrm.so', 'usr/lib/libc.so.6'):
        if not (root / header).is_file():
            parser.error(f'root lacks {header}; use a disposable Arch root with libdrm')

    target = root / 'tmp/guest-rdmetal-probe'
    group = Path(tempfile.mkdtemp(prefix='shim-rdmetal-probe-', dir='/tmp'))
    env = os.environ.copy()
    env.update(RDDRM='1', RDUID0='1', RDSOCKETGROUP=str(group))

    def preexec():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        os.setsid()

    try:
        # clang's PIE driver wants a gcc crtbegin/crtend the Arch root does not
        # ship, so link the guest by hand against the root's glibc + libdrm.
        subprocess.run([
            'clang', '--target=aarch64-linux-gnu', '--sysroot=' + str(root),
            '-I' + str(root / 'usr/include/libdrm'), '-fuse-ld=lld',
            '-fPIE', '-pie', '-nostdlib',
            str(root / 'usr/lib/crt1.o'), str(root / 'usr/lib/crti.o'),
            str(base / 'tests/guest-rdmetal-probe.c'),
            '-L' + str(root / 'usr/lib'), '-ldrm', '-lc',
            str(root / 'usr/lib/crtn.o'),
            '-o', str(target),
        ], check=True, timeout=30)

        proc = subprocess.Popen([str(shim), str(root), '/tmp/guest-rdmetal-probe'],
                                env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, preexec_fn=preexec)
        try:
            output, _ = proc.communicate(timeout=20)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            output, _ = proc.communicate()
            raise RuntimeError('rdmetal probe timed out: ' + output[-1200:])

        if proc.returncode or not re.search(r'RDMETAL_PROBE_OK', output):
            raise RuntimeError(f'rdmetal probe failed (exit {proc.returncode}):\n{output[-3000:]}')
        # Surface the enumerated shape so a regression is readable, not just "OK".
        for line in output.splitlines():
            if line.startswith(('cap ', 'resources ', 'connector ', 'encoder ',
                                'crtc ', 'planes ', 'plane ', 'dumb ', 'addfb2 ',
                                'prime ', 'RDMETAL_PROBE_OK')):
                print(line)
    finally:
        target.unlink(missing_ok=True)
        shutil.rmtree(group)


if __name__ == '__main__':
    main()
