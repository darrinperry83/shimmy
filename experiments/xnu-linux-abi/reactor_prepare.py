"""Opt-in epoll/timerfd/signalfd batch on native XNU descriptor/readiness state."""
from pathlib import Path
import subprocess
from elf_prepare import replace_once
HERE=Path(__file__).resolve().parent

def overlay(root,edits):
    root=Path(root)
    path=root/'bsd/kern/rdxnu_linux_exec.h';before,data=edits[path];s=data.decode()
    s=replace_once(s,'bool rdxnu_linux_dispatch(arm_saved_state_t *state, struct proc *p);',
        (HERE/'reactor/dispatch.inc').read_text()+'\nbool rdxnu_linux_dispatch(arm_saved_state_t *state, struct proc *p);')
    s=replace_once(s,'    if (nr == 25 && get_saved_state_reg(state,1)==3',
        '    if(nr==57 && rd_linux_epoll_close_alias(p,(int)get_saved_state_reg(state,0)))value=-95;\n    else if(nr==221 && rd_linux_epoll_exec_guard(p))value=-95;\n    else if(nr==20||nr==21||nr==22||nr==74||nr==85||nr==86||nr==87||nr==98)value=rd_linux_reactor(p,state,nr);\n    else if(nr==25 && get_saved_state_reg(state,1)==3 && rd_linux_reactor_getfl(p,(int)get_saved_state_reg(state,0),&value)) {}\n    else if (nr == 25 && get_saved_state_reg(state,1)==3')
    s=replace_once(s,'    struct rd_linux_proc_state *s = p->p_stats->ps_rd_linux;\n    if (!s) return;\n    p->p_stats->ps_rd_linux = NULL;',
        '    struct rd_linux_proc_state *s = rd_signal_state_detach(p);\n    if (!s) return;')
    s=replace_once(s,'    child->p_stats->ps_rd_linux = s;','    rd_signal_state_assign(child,s);')
    s=replace_once(s,'        p->p_stats->ps_rd_linux = s;','        rd_signal_state_assign(p,s);')
    edits[path]=(before,s.encode())
    path=root/'bsd/kern/rdxnu_linux_state.h';before,data=edits[path];s=data.decode()
    s=replace_once(s,'struct rd_linux_proc_state {', 'struct rd_signal_meta {uint32_t pid,uid;int32_t code,status;};\nstruct rd_linux_proc_state {')
    s=replace_once(s,'    uint64_t heap_base, heap_current;','    uint64_t heap_base, heap_current;\n    struct rd_signal_meta signals[32];')
    s+='\nstruct rd_linux_proc_state *rd_signal_state_detach(struct proc *);\nvoid rd_signal_state_assign(struct proc *,struct rd_linux_proc_state *);\n'
    edits[path]=(before,s.encode())
    path=root/'bsd/sys/file_internal.h';before,data=edits[path];s=data.decode().replace('DTYPE_RDXNU_EVENTFD = 12 /*','DTYPE_RDXNU_EVENTFD = 12, /*')
    s=replace_once(s,'} file_type_t;', '\tDTYPE_RDXNU_EPOLL = 13,\n\tDTYPE_RDXNU_TIMERFD = 14,\n\tDTYPE_RDXNU_SIGNALFD = 15\n} file_type_t;');edits[path]=(before,s.encode())
    path=root/'bsd/sys/event_private.h';before,data=edits[path];s=replace_once(data.decode(),'#define EVFILTID_MAX               (EVFILT_SYSCOUNT + 23)', '#define EVFILTID_RDXNU_SIGNALFD    (EVFILT_SYSCOUNT + 23)\n#define EVFILTID_MAX               (EVFILT_SYSCOUNT + 24)');edits[path]=(before,s.encode())
    path=root/'bsd/kern/kern_event.c';before,data=edits[path];s=data.decode()
    s=replace_once(s,'extern const struct filterops rd_eventfd_filtops;', 'extern const struct filterops rd_eventfd_filtops;\nextern const struct filterops rd_sigfd_filtops;\nvoid rd_linux_epoll_fdclose(struct proc *,int);')
    s=replace_once(s,'\t[EVFILTID_RDXNU_EVENTFD]', '\t[EVFILTID_RDXNU_SIGNALFD]       = &rd_sigfd_filtops,\n\t[EVFILTID_RDXNU_EVENTFD]')
    s=replace_once(s,'knote_fdclose(struct proc *p, int fd)\n{', 'knote_fdclose(struct proc *p, int fd)\n{\n\trd_linux_epoll_fdclose(p,fd);')
    s+='\n'+(HERE/'reactor/epoll.inc').read_text();edits[path]=(before,s.encode())
    path=root/'bsd/kern/kern_descrip.c';before,data=edits[path];s=data.decode()
    s=replace_once(s,'static int\nfstat(proc_t p,',(HERE/'reactor/timerfd.inc').read_text()+'\nstatic int\nfstat(proc_t p,')
    s=replace_once(s,'\tcase DTYPE_RDXNU_EVENTFD:', '\tcase DTYPE_RDXNU_EPOLL:\n\tcase DTYPE_RDXNU_TIMERFD:\n\tcase DTYPE_RDXNU_SIGNALFD:\n\tcase DTYPE_RDXNU_EVENTFD:')
    s=replace_once(s,'void\nfdt_exec(proc_t p,', 'extern void rd_linux_epoll_owner_cleanup(struct proc *);\n\nvoid\nfdt_exec(proc_t p,')
    if s.count('\tknotes_dealloc(p);')!=2:raise ValueError('fd-table lifecycle changed')
    s=s.replace('\tknotes_dealloc(p);','\trd_linux_epoll_owner_cleanup(p);\n\tknotes_dealloc(p);')
    edits[path]=(before,s.encode())
    path=root/'bsd/kern/sys_generic.c';before,data=edits[path];s=data.decode()
    s=replace_once(s,'\t/* JMM - all this P_SELECT stuff is bogus */', '''\tif(os_atomic_load(&p->p_ladvflag,relaxed)&P_RDXNU_LINUX){
\t\tfor(u_int i=0;i<nfds;i++)if(fds[i].fd>=0){struct fileproc *fp;
\t\t\tif(!fp_lookup(p,fds[i].fd,&fp,0)){file_type_t type=FILEGLOB_DTYPE(fp->fp_glob);bool ep=type==DTYPE_RDXNU_EPOLL||((type==DTYPE_RDXNU_TIMERFD||type==DTYPE_RDXNU_SIGNALFD)&&(fds[i].events&4));
\t\t\t\tfp_drop(p,fds[i].fd,fp,0);if(ep){error=ENOTSUP;goto out;}}
\t\t}
\t}
\t/* JMM - all this P_SELECT stuff is bogus */''');edits[path]=(before,s.encode())
    path=root/'bsd/kern/kern_proc.c';before=path.read_bytes()
    if before!=subprocess.check_output(['git','-C',str(root),'show','HEAD:bsd/kern/kern_proc.c']):raise ValueError('proc source modified')
    s=replace_once(before.decode(),'\t\tif (kn->kn_filtid == (uint8_t)~EVFILT_PROC) {',
        '\t\tif(kn->kn_filtid==EVFILTID_RDXNU_SIGNALFD){\n\t\t\tknote_kn_hook_set_raw(kn,NULL);\n\t\t} else if (kn->kn_filtid == (uint8_t)~EVFILT_PROC) {')
    s=replace_once(s,'\t\tkn->kn_proc = PROC_NULL;\n\t\tKNOTE_DETACH(&p->p_klist, kn);',
        '\t\tif(kn->kn_filtid==EVFILTID_RDXNU_SIGNALFD)knote_kn_hook_set_raw(kn,NULL);\n\t\telse kn->kn_proc = PROC_NULL;\n\t\tKNOTE_DETACH(&p->p_klist, kn);')
    edits[path]=(before,s.encode())
    path=root/'bsd/kern/kern_sig.c';before=path.read_bytes()
    if before!=subprocess.check_output(['git','-C',str(root),'show','HEAD:bsd/kern/kern_sig.c']):raise ValueError('signal source modified')
    s=before.decode();s=replace_once(s,'static int      filt_signal(', 'extern void rd_linux_signal_post(struct proc *,int,bool);\nextern bool rd_linux_keep_blocked_default(struct proc *,int);\nstatic int      filt_signal(')
    s=replace_once(s,'\tuth->uu_siglist |= mask;\n\n\t/*', '\tbool rd_already_pending=(uth->uu_siglist&mask)!=0;\n\tuth->uu_siglist |= mask;\n\trd_linux_signal_post(sig_proc,signum,rd_already_pending);\n\n\t/*')
    s=replace_once(s,'(sig_proc->p_sigignore & mask)) {','(sig_proc->p_sigignore & mask) && !rd_linux_keep_blocked_default(sig_proc,signum)) {')
    s=replace_once(s,'if (sig_proc->p_sigignore & mask) {','if ((sig_proc->p_sigignore & mask) && !rd_linux_keep_blocked_default(sig_proc,signum)) {')
    s+='\n'+(HERE/'reactor/signalfd.inc').read_text()
    s+='\n'+(HERE/'reactor/futex.inc').read_text();edits[path]=(before,s.encode());return edits
