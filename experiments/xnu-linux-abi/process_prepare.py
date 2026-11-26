"""Opt-in native fork/waitid experiment layered on the proven path checkpoint."""
from pathlib import Path
from elf_prepare import replace_once
HERE=Path(__file__).resolve().parent

def overlay(root,edits):
    root=Path(root)
    path=root/'bsd/kern/rdxnu_linux_exec.h';before,data=edits[path];s=data.decode()
    s=replace_once(s,'bool rdxnu_linux_dispatch(arm_saved_state_t *state, struct proc *p);',
        (HERE/'process/dispatch.inc').read_text()+'\nbool rdxnu_linux_dispatch(arm_saved_state_t *state, struct proc *p);')
    s=replace_once(s,'    if (nr == 291)',
        '    if (nr == 95 || nr == 129 || nr == 173 || nr == 220 || nr == 221) value=rd_linux_process(p,state,nr);\n    else if (nr == 291)')
    s=replace_once(s,'    case EPERM: return 1;','    case ESRCH: return 3; case ECHILD: return 10; case ENOEXEC: return 8; case ETXTBSY: return 26;\n    case EPERM: return 1;')
    edits[path]=(before,s.encode())
    path=root/'bsd/kern/rdxnu_linux_state.h';before,data=edits[path]
    edits[path]=(before,data+b'\nvoid rd_linux_fork_child(struct proc *,struct proc *,thread_t);\n')
    path=root/'bsd/kern/kern_fork.c';before,data=edits[path];s=data.decode()
    s=replace_once(s,'\t\tthread_set_child(child_thread, proc_getpid(child_proc));',
        '\t\tthread_set_child(child_thread, proc_getpid(child_proc));\n\t\tif(!spawn)rd_linux_fork_child(parent_proc,child_proc,child_thread);')
    edits[path]=(before,s.encode())
    path=root/'bsd/kern/kern_exit.c';before,data=edits[path];s=data.decode()
    s=replace_once(s,'static int\ncopyoutsiginfo(',
        (HERE/'process/wait_copyout.h').read_text()+'\nstatic int\ncopyoutsiginfo(')
    s=replace_once(s,'copyoutsiginfo(user_siginfo_t *native, boolean_t is64, user_addr_t uaddr)\n{',
        'copyoutsiginfo(user_siginfo_t *native, boolean_t is64, user_addr_t uaddr)\n{\n\tif(is64 && rd_linux_wait_caller())return rd_linux_wait_copyout(native,uaddr);')
    anchor='\t\t\tsiginfo.si_pid = proc_getpid(p);'
    if s.count(anchor)!=2:raise ValueError('unexpected native waitid event sites')
    s=s.replace(anchor,anchor+'\n\t\t\tif(rd_linux_wait_caller())siginfo.si_uid=kauth_cred_getruid(proc_ucred_unsafe(p));')
    s=replace_once(s,'\t\t * invocation, then check for a non-zero value afterwards.\n\t\t */\n\t\treturn 0;',
        '\t\t * invocation, then check for a non-zero value afterwards.\n\t\t */\n\t\tif(rd_linux_wait_caller())return rd_linux_wait_copyout(NULL,uap->infop);\n\t\treturn 0;')
    s=replace_once(s,'\tif ((error = msleep0(q, &proc_list_mlock,',
        '\t/* Linux SVC keeps local arguments on the kernel stack. Do not enter\n\t * a BSD syscall continuation with pointers into a discarded stack. */\n\tif(rd_linux_wait_caller()){\n\t\terror=msleep(q,&proc_list_mlock,PWAIT|PCATCH|PDROP,"linux_waitid",0);\n\t\tif(error)return error;\n\t\tgoto loop;\n\t}\n\tif ((error = msleep0(q, &proc_list_mlock,')
    edits[path]=(before,s.encode());return edits
