#!/usr/bin/env python3
"""Exercise installed rdmetal through guest EGL and verify actual broker pixels."""
import os
import re
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
root = Path(os.environ.get('ROOTFS', repo / 'rootfs')).resolve()
clang = os.environ.get('CLANG', '/opt/homebrew/opt/llvm/bin/clang')
lld = os.environ.get('LLD', '/opt/homebrew/opt/lld/bin/ld.lld')
with tempfile.TemporaryDirectory(prefix='depth-egl-', dir=root / 'tmp') as work:
    binary = Path(work) / 'probe'
    subprocess.run([
        clang, '--target=aarch64-linux-gnu', '-fuse-ld=' + lld,
        '--sysroot=' + str(root), '-nostdlib', str(root / 'usr/lib/crt1.o'),
        str(root / 'usr/lib/crti.o'), str(repo / 'tests/guest-depth-stencil.c'),
        '-L' + str(root / 'usr/lib'), '-lEGL', '-lGLESv2', '-lc',
        str(root / 'usr/lib/crtn.o'), '-o', str(binary),
    ], check=True)
    env = os.environ.copy()
    env.update(RDSKIP_RUNCLEAR='1', RDMETAL_TRACE_DRAW='1',
               RDENV_RDMETAL_TRACE_DRAW='1')
    result = subprocess.run([str(repo / 'shim_cli'), str(root),
                             '/' + str(binary.relative_to(root))],
                            env=env, capture_output=True, text=True, timeout=45)
    log = result.stdout + result.stderr
    pixels = re.findall(r'RDMETAL_FIXTURE_SAMPLED_PIXEL .*?bgra=(\d+,\d+,\d+,\d+)', log)
    expected = ['0,0,255,255'] * 3 + ['0,255,0,255'] * 3
    if (result.returncode or pixels != expected or
            'ALPHA_GLYPH_EGL_OK' not in log or 'FIRST_FAILURE' in log):
        raise SystemExit(f'Guest EGL depth/stencil failed ({result.returncode}), '
                         f'pixels={pixels}:\n{log[-9000:]}')
    print('PASS guest EGL: real shader color, depth occlusion, stencil reject, stencil clear, alpha glyph upload, indexed strips/fans')
