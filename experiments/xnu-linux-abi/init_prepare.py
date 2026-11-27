"""Opt-in native subreaper step toward a Linux init domain; no PID override."""
from pathlib import Path
from elf_prepare import replace_once

HERE = Path(__file__).resolve().parent


def overlay(root, edits):
    root = Path(root)
    path = root / 'bsd/kern/rdxnu_linux_state.h'
    before, data = edits[path]
    text = replace_once(data.decode(), '    uint64_t heap_base, heap_current;',
                        '    uint64_t heap_base, heap_current;\n    uint32_t child_subreaper;')
    text += '\nuint32_t rd_linux_subreaper_get(struct proc *);\nint rd_linux_subreaper_set(struct proc *,uint32_t);\n'
    edits[path] = (before, text.encode())

    path = root / 'bsd/kern/kern_sig.c'
    before, data = edits[path]
    text = data.decode() + '''
/* Serialize sidecar reads with detach during exec, using its existing lock. */
uint32_t rd_linux_subreaper_get(struct proc *p)
{
    lck_spin_lock(&rd_sigmeta_lock);
    struct rd_linux_proc_state *s=p->p_stats?p->p_stats->ps_rd_linux:NULL;
    uint32_t result=s?s->child_subreaper:0;
    lck_spin_unlock(&rd_sigmeta_lock);
    return result;
}
int rd_linux_subreaper_set(struct proc *p,uint32_t value)
{
    lck_spin_lock(&rd_sigmeta_lock);
    struct rd_linux_proc_state *s=p->p_stats?p->p_stats->ps_rd_linux:NULL;
    if(s)s->child_subreaper=value!=0;
    lck_spin_unlock(&rd_sigmeta_lock);
    return s?0:EINVAL;
}
'''
    edits[path] = (before, text.encode())

    path = root / 'bsd/kern/rdxnu_linux_exec.h'
    before, data = edits[path]
    text = replace_once(data.decode(), '    rd_linux_state_free(p);\n    uint64_t mask=0;',
                        '    uint32_t subreaper=rd_linux_subreaper_get(p);\n    rd_linux_state_free(p);\n    uint64_t mask=0;')
    text = replace_once(text, '        s->heap_base = s->heap_current = base;',
                        '        s->heap_base = s->heap_current = base;\n        s->child_subreaper=subreaper;')
    text = replace_once(text, '    if (nr==167 && (a0==15 || a0==16)) {', '''    if (nr==167 && a0==36) {
        int error=rd_linux_subreaper_set(p,a1!=0);
        return error?-rd_linux_errno(error):0;
    }
    if (nr==167 && a0==37) {
        uint32_t value=rd_linux_subreaper_get(p);
        return copyout(&value,a1,sizeof(value))?-14:0;
    }
    if (nr==167 && (a0==15 || a0==16)) {''')
    edits[path] = (before, text.encode())

    # A successful exec may replace the native proc with its shadow. Copy the
    # property only at the successful switch, never into ordinary fork children.
    path = root / 'bsd/kern/kern_exec.c'
    before, data = edits[path]
    text = replace_once(data.decode(), '\t\tsession_replace_leader(old_proc, new_proc);',
                        '\t\t(void)rd_linux_subreaper_set(new_proc,rd_linux_subreaper_get(old_proc));\n\t\tsession_replace_leader(old_proc, new_proc);')
    edits[path] = (before, text.encode())

    path = root / 'bsd/kern/kern_exit.c'
    before, data = edits[path]
    text = replace_once(data.decode(), 'static int initproc_spawned = 0;',
                        (HERE / 'init/subreaper.inc').read_text() + '\nstatic int initproc_spawned = 0;')
    text = replace_once(text, '\tproc_childdrainstart(p);\n\twhile ((q = p->p_children.lh_first) != NULL) {',
                        '\tproc_t rd_adopter=rd_linux_subreaper_find_locked(p);\n\tproc_childdrainstart(p);\n\twhile ((q = p->p_children.lh_first) != NULL) {\n\t\tif(rd_linux_subreaper_adopt_locked(rd_adopter,q))continue;')
    text = replace_once(text, '\tproc_childdrainend(p);\n\tproc_list_unlock();',
                        '\tproc_childdrainend(p);\n\tproc_list_unlock();\n\tif(rd_adopter!=PROC_NULL)proc_rele(rd_adopter);')
    edits[path] = (before, text.encode())
    return edits
