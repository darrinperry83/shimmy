"""Stage the freestanding Linux-core test in a separate VMAPPLE build only."""
import hashlib
import json
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent.parent / 'linux-on-xnu/xnu-host'
SOURCE = HERE.parent / 'out/darwin-source'
CORE = HERE.parent / 'out/xnu-core/linux-core.o'


def verified_core():
    report = json.loads(CORE.with_name('build-result.json').read_text())
    pin = subprocess.check_output(['git', '-C', str(SOURCE), 'rev-parse', 'HEAD'], text=True).strip()
    if report['source_commit'] != pin or pin != 'c4d0895ead9badb52f7bbffe3d529c61c8fc9485':
        raise ValueError('Linux source/core pin mismatch')
    if report['architecture'] != 'arm64e' or report['posix_host_or_libsystem_objects_linked']:
        raise ValueError('expected freestanding arm64e Linux core')
    if hashlib.sha256(CORE.read_bytes()).hexdigest() != report['core_sha256']:
        raise ValueError('Linux core changed since build')
    return CORE, report


def overlay(root, edits):
    verified_core()
    root = Path(root)
    for name in ('rdxnu_linux_host.c', 'rdxnu_linux_jump.s'):
        path = root / 'osfmk/kern' / name
        if path.exists():
            raise ValueError(f'refusing to replace existing {path}')
        edits[path] = (None, (HERE / name).read_bytes())
    path = root / 'osfmk/kern/rdxnu_lkl_api.h'
    if path.exists():
        raise ValueError(f'refusing to replace existing {path}')
    api = (SOURCE / 'tools/lkl/darwin/profile/host-include/lkl/asm/host_ops.h').read_bytes()
    edits[path] = (None, b'#define lkl_init rdl_lkl_init\n#define lkl_start_kernel rdl_lkl_start_kernel\n#define lkl_cleanup rdl_lkl_cleanup\n' + api)
    path = root / 'osfmk/conf/files'
    before = path.read_bytes()
    edits[path] = (before, before + b'\nosfmk/kern/rdxnu_linux_host.c standard\nosfmk/kern/rdxnu_linux_jump.s standard\n')
    path = root / 'bsd/kern/rdxnu_linux_exec.h'
    before, source = edits[path]
    edits[path] = (before, source + b'\n' + (HERE / 'control.inc').read_bytes())
    return edits
