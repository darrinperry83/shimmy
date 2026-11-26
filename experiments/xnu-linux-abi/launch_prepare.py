"""Opt-in service launch controls on native descriptors, limits and processes."""
from pathlib import Path
import subprocess
from elf_prepare import replace_once
HERE=Path(__file__).resolve().parent

def overlay(root,edits):
    root=Path(root)
    path=root/'bsd/kern/rdxnu_linux_exec.h';before,data=edits[path];s=data.decode()
    s=replace_once(s,'bool rdxnu_linux_dispatch(arm_saved_state_t *state, struct proc *p);',
        (HERE/'launch/dispatch.inc').read_text()+'\nbool rdxnu_linux_dispatch(arm_saved_state_t *state, struct proc *p);')
    s=replace_once(s,'    if(nr==57 && rd_linux_epoll_close_alias',
        '    if(nr==23||nr==24||nr==59||nr==154||nr==155||nr==156||nr==157||nr==163||nr==164||nr==166||nr==260||nr==261||(nr==25&&(get_saved_state_reg(state,1)==4||get_saved_state_reg(state,1)==1030)))value=rd_linux_launch(p,state,nr);\n    else if(nr==57 && rd_linux_epoll_close_alias')
    edits[path]=(before,s.encode())
    path=root/'bsd/kern/kern_descrip.c';before,data=edits[path]
    edits[path]=(before,data+b'\nextern bool rd_linux_epoll_close_alias(struct proc *,int);\n'+(HERE/'launch/descriptors.inc').read_bytes())
    path=root/'bsd/kern/kern_resource.c';before=path.read_bytes()
    if before!=subprocess.check_output(['git','-C',str(root),'show','HEAD:bsd/kern/kern_resource.c']):raise ValueError('resource source modified')
    edits[path]=(before,before+b'\n'+(HERE/'launch/limits.inc').read_bytes())
    path=root/'bsd/kern/kern_exit.c';before,data=edits[path];s=data.decode()
    s=replace_once(s,'\t\t\t\tstatus = 0xffff & p->p_xstat;   /* convert to int */',
        '\t\t\t\tstatus = 0xffff & p->p_xstat;   /* convert to int */\n\t\t\t\tif(rd_linux_wait_caller()&&WIFSIGNALED(status))status=(status&0x80)|rd_linux_wait_signal(WTERMSIG(status));')
    s=replace_once(s,'\t\t\t\t\t\tmunge_user64_rusage(&p->p_ru->ru, &my_rusage);',
        '\t\t\t\t\t\tmunge_user64_rusage(&p->p_ru->ru, &my_rusage);\n\t\t\t\t\t\tif(rd_linux_wait_caller())my_rusage.ru_maxrss/=1024;')
    anchor='\tif ((error = msleep0((caddr_t)q, &proc_list_mlock, PWAIT | PCATCH | PDROP, "wait", 0, wait1continue))) {'
    s=replace_once(s,anchor,'\tif(rd_linux_wait_caller()){\n\t\terror=msleep(q,&proc_list_mlock,PWAIT|PCATCH|PDROP,"linux_wait4",0);\n\t\tif(error)return error;\n\t\tgoto loop;\n\t}\n'+anchor)
    edits[path]=(before,s.encode());return edits
