"""Temporary file ABI overlay: native XNU lookup, Linux metadata copyout."""
from pathlib import Path
import subprocess
from elf_prepare import replace_once

HERE = Path(__file__).resolve().parent


def overlay(root, edits):
    root = Path(root)
    path = root / 'bsd/kern/rdxnu_linux_exec.h'
    before, data = edits[path]
    text = replace_once(data.decode(), 'bool rdxnu_linux_dispatch(arm_saved_state_t *state, struct proc *p);',
                        (HERE / 'files/dispatch.inc').read_text() + '\nbool rdxnu_linux_dispatch(arm_saved_state_t *state, struct proc *p);')
    text = replace_once(text, '    if (nr == 222 || nr == 226 || nr == 215)',
                        '    if (nr == 25 || nr == 56 || nr == 57 || nr == 62 || nr == 78 || nr == 79 || nr == 80) value = rd_linux_files(p,state,nr);\n    else if (nr == 222 || nr == 226 || nr == 215)')
    edits[path] = (before,text.encode())
    for relative, anchor, old, new in [
        ('bsd/kern/kern_descrip.c', 'static int\nfstat(proc_t p,',
         '\t\terror = copyout(sbp, ub, my_size);',
         '\t\terror = isstat64 && (os_atomic_load(&p->p_ladvflag, relaxed) & P_RDXNU_LINUX)\n\t\t    ? rd_linux_stat_copyout(&source.sb64, ub) : copyout(sbp, ub, my_size);'),
        ('bsd/vfs/vfs_syscalls.c', 'static int\nfstatat_internal(vfs_context_t ctx,',
         '\tif ((error = copyout(sbp, ub, my_size)) != 0) {',
         '\tif ((error = isstat64 && (os_atomic_load(&vfs_context_proc(ctx)->p_ladvflag, relaxed) & P_RDXNU_LINUX)\n\t    ? rd_linux_stat_copyout(&source.sb64, ub) : copyout(sbp, ub, my_size)) != 0) {'),
    ]:
        path = root / relative
        before = path.read_bytes()
        expected = subprocess.check_output(['git','-C',str(root),'show',f'HEAD:{relative}'])
        if before != expected:
            raise ValueError(f'file ABI requires unmodified {relative}')
        text = replace_once(before.decode(),anchor,'#include "rdxnu_linux_stat.h"\n\n' + anchor)
        text = replace_once(text,old,new)
        edits[path] = (before,text.encode())
    for directory in ('bsd/kern','bsd/vfs'):
        path = root / directory / 'rdxnu_linux_stat.h'
        if path.exists(): raise ValueError(f'refusing to replace {path}')
        edits[path] = (None,(HERE / 'files/stat_copyout.h').read_bytes())
    return edits
