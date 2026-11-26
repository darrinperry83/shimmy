"""Opt-in native eventfd and ppoll; no native structure layout changes."""
from pathlib import Path
import subprocess
from elf_prepare import replace_once
HERE=Path(__file__).resolve().parent

def overlay(root,edits):
    root=Path(root)
    path=root/'bsd/kern/rdxnu_linux_exec.h';before,data=edits[path]
    s=replace_once(data.decode(),'bool rdxnu_linux_dispatch(arm_saved_state_t *state, struct proc *p);',
        (HERE/'events/dispatch.inc').read_text()+'\nbool rdxnu_linux_dispatch(arm_saved_state_t *state, struct proc *p);')
    s=replace_once(s,'    if (nr == 95 ||', '    if (nr == 19 || nr == 73) value=rd_linux_events(p,state,nr);\n    else if (nr == 95 ||')
    s=replace_once(s,'    if (nr == 19 || nr == 73)', '    if (nr == 25 && get_saved_state_reg(state,1)==3 && rd_linux_eventfd_getfl(p,(int)get_saved_state_reg(state,0),&value)) {}\n    else if (nr == 19 || nr == 73)')
    edits[path]=(before,s.encode())
    replacements={
        'bsd/sys/file_internal.h': [('\tDTYPE_NEXUS             /* Skywalk Nexus */', '\tDTYPE_NEXUS,            /* Skywalk Nexus */\n\tDTYPE_RDXNU_EVENTFD = 12 /* Opt-in Linux anonymous counter */')],
        'bsd/sys/event_private.h': [('#define EVFILTID_MAX               (EVFILT_SYSCOUNT + 22)', '#define EVFILTID_RDXNU_EVENTFD     (EVFILT_SYSCOUNT + 22)\n#define EVFILTID_MAX               (EVFILT_SYSCOUNT + 23)')],
        'bsd/kern/kern_event.c': [('static const struct filterops * const sysfilt_ops[EVFILTID_MAX] = {', 'extern const struct filterops rd_eventfd_filtops;\nstatic const struct filterops * const sysfilt_ops[EVFILTID_MAX] = {'), ('\t[EVFILTID_KQREAD]', '\t[EVFILTID_RDXNU_EVENTFD]        = &rd_eventfd_filtops,\n\t[EVFILTID_KQREAD]')],
    }
    # Do not modify generic poll's descriptor snapshot before copyin. Validate
    # that snapshot once; native core poll bits match AArch64 Linux exactly.
    replacements['bsd/kern/sys_generic.c']=[('\t/* JMM - all this P_SELECT stuff is bogus */', '''\tif(os_atomic_load(&p->p_ladvflag,relaxed)&P_RDXNU_LINUX){
\t\tfor(u_int i=0;i<nfds;i++)if(fds[i].fd>=0 && ((fds[i].events&~0x3d) || !(fds[i].events&5))){
\t\t\terror=ENOTSUP;goto out;
\t\t}
\t}
\t/* JMM - all this P_SELECT stuff is bogus */''')]
    for relative,changes in replacements.items():
        path=root/relative;before=path.read_bytes()
        if before!=subprocess.check_output(['git','-C',str(root),'show',f'HEAD:{relative}']):raise ValueError(relative)
        s=before.decode()
        for old,new in changes:s=replace_once(s,old,new)
        edits[path]=(before,s.encode())
    path=root/'bsd/kern/kern_descrip.c';before,data=edits[path]
    s=replace_once(data.decode(),'static int\nfstat(proc_t p,',
        (HERE/'events/eventfd.h').read_text()+'\nstatic int\nfstat(proc_t p,')
    # Truthful unsupported metadata until an anonymous inode view exists.
    s=replace_once(s,'\tcase DTYPE_KQUEUE:\n\t\terror = kqueue_stat',
        '\tcase DTYPE_RDXNU_EVENTFD:\n\t\terror=ENOTSUP;goto out;\n\tcase DTYPE_KQUEUE:\n\t\terror = kqueue_stat')
    edits[path]=(before,s.encode());return edits
