"""XNU-backed Linux memory personality overlay; independent of hosted Linux."""
from pathlib import Path
import subprocess
from elf_prepare import replace_once
HERE = Path(__file__).resolve().parent


def overlay(root, edits):
    path = Path(root) / 'bsd/kern/rdxnu_linux_exec.h'
    before, source = edits[path]
    text = replace_once(source.decode(), 'bool rdxnu_linux_dispatch(arm_saved_state_t *state, struct proc *p);',
                        (HERE / 'memory/dispatch.inc').read_text() + '\nbool rdxnu_linux_dispatch(arm_saved_state_t *state, struct proc *p);')
    text = replace_once(text, '    if (nr == 63) {',
                        '    if (nr == 222 || nr == 226 || nr == 215) value = rd_linux_memory(p,state,nr);\n    else if (nr == 63) {')
    edits[path] = (before, text.encode())
    # Preserve the native mmap descriptor lookup/reference lifetime. A separate
    # preflight fstat would race a concurrent close/reuse of the descriptor.
    path = Path(root) / 'bsd/kern/kern_mman.c'
    before = path.read_bytes()
    expected = subprocess.check_output(['git','-C',str(root),'show','HEAD:bsd/kern/kern_mman.c'])
    if before != expected:
        raise ValueError('memory requires unmodified kern_mman.c')
    text = replace_once(before.decode(),
        '''\t\tif (vp->v_type != VREG && vp->v_type != VCHR) {
\t\t\t(void)vnode_put(vp);
\t\t\terror = EINVAL;''',
        '''\t\tif (vp->v_type != VREG && vp->v_type != VCHR) {
\t\t\t(void)vnode_put(vp);
\t\t\t/* Linux mmap rejects unsupported vnode kinds with ENODEV. */
\t\t\terror = (os_atomic_load(&p->p_ladvflag, relaxed) & P_RDXNU_LINUX) ? ENODEV : EINVAL;''')
    edits[path] = (before,text.encode())
    return edits
