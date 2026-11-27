"""Isolated domain ownership/boundaries on native IDs; not a PID namespace."""
from pathlib import Path
from elf_prepare import replace_once

HERE=Path(__file__).resolve().parent


def overlay(root,edits):
    root=Path(root)
    path=root/'bsd/kern/rdxnu_linux_state.h';before,data=edits[path]
    text=data.decode()+'\n'+(HERE/'init/domain.h').read_text()
    edits[path]=(before,text.encode())

    path=root/'bsd/kern/kern_sig.c';before,data=edits[path]
    edits[path]=(before,data+b'\n'+(HERE/'init/domain.inc').read_bytes())

    path=root/'bsd/kern/rdxnu_linux_exec.h';before,data=edits[path];text=data.decode()
    text=replace_once(text,'    if(nr==220){',
                      '    if(nr==220){\n        if(rd_linux_domain_closing(p))return -3;')
    text=replace_once(text,'    target->rd_linux_clear_tid=0;target->rd_linux_enabled=1;',
                      '    target->rd_linux_clear_tid=0;target->rd_linux_enabled=1;\n    if(rd_linux_domain_closing(child))psignal(child,SIGKILL);')
    text=replace_once(text,'    if(nr==173)return proc_ppid(p);',
                      '    if(nr==173)return rd_linux_domain_leader(p)?0:proc_ppid(p);')
    text=replace_once(text,'        error=kill(p,&args,result);', '''        proc_t target=proc_find(args.pid);
        if(target==PROC_NULL)return -3;
        if(!rd_linux_domain_target(p,target)){proc_rele(target);return -3;}
        error=kill(p,&args,result);proc_rele(target);''')
    # Current limited session/group and prlimit interfaces retain native IDs,
    # but may not use a domain caller to query/manipulate an outside process.
    text=replace_once(text,'    if(nr==59)error=rd_linux_pipe2', '''    if(rd_linux_domain_id(p)&&(nr==154||nr==155||nr==156||nr==261)){
        if((int)a0>0){proc_t target=proc_find((int)a0);
            if(target==PROC_NULL)return -3;
            bool allowed=rd_linux_domain_target(p,target),self=target==p;proc_rele(target);
            if(!allowed)return -3;
            if(!self)return -95; /* Other member queries await held-reference dispatch. */
        }
        if(nr==154&&(int)a1>0){proc_t leader=proc_find((int)a1);
            if(leader==PROC_NULL)return -3;
            bool allowed=rd_linux_domain_target(p,leader),self=leader==p;proc_rele(leader);
            if(!allowed)return -3;
            if(!self)return -95;
        }
    }
    if(nr==59)error=rd_linux_pipe2''')
    edits[path]=(before,text.encode())

    path=root/'bsd/kern/kern_fork.c';before,data=edits[path];text=data.decode()
    text=replace_once(text,'bad:\n\treturn child_proc;',
                      'bad:\n\tif(child_proc&&!in_exec&&rd_linux_domain_inherit(parent_proc,child_proc)){forkproc_free(child_proc);child_proc=NULL;}\n\treturn child_proc;')
    text=replace_once(text,'\trd_linux_state_free(p);\n\tzfree(proc_stats_zone, p->p_stats);',
                      '\trd_linux_domain_forget(p);\n\trd_linux_state_free(p);\n\tzfree(proc_stats_zone, p->p_stats);')
    edits[path]=(before,text.encode())

    path=root/'bsd/kern/kern_exec.c';before,data=edits[path];text=data.decode()
    text=replace_once(text,'\tbool rd_linux = !memcmp(imgp->ip_vdata, "\\177ELF", 4);',
                      '\tbool rd_linux = !memcmp(imgp->ip_vdata, "\\177ELF", 4);\n\tif(rd_linux_domain_id(vfs_context_proc(imgp->ip_vfs_context))&&(!rd_linux||rd_linux_domain_closing(vfs_context_proc(imgp->ip_vfs_context))))return ENOEXEC;')
    edits[path]=(before,text.encode())

    path=root/'bsd/kern/kern_exit.c';before,data=edits[path];text=data.decode()
    text=replace_once(text,'\t(void)acct_process(p);',
                      '\t(void)acct_process(p);\n\trd_linux_domain_exit(p);')
    text=replace_once(text,'\tpgrp_rele(pg);\n\tfdt_destroy(child);',
                      '\tif(!shadow_proc)rd_linux_domain_forget(child);\n\tpgrp_rele(pg);\n\tfdt_destroy(child);')
    text=replace_once(text,'        if (rd_linux_subreaper_get(ancestor) && !(ancestor->p_lflag & P_LEXIT)) {',
                      '        if(!rd_linux_domain_same(exiting,ancestor))break;\n        if ((rd_linux_subreaper_get(ancestor)||rd_linux_domain_leader(ancestor)) && !(ancestor->p_lflag & P_LEXIT)) {')
    text=replace_once(text,'    if (adopter == PROC_NULL || (child->p_lflag & P_LTRACED) ||',
                      '    if (adopter == PROC_NULL || !rd_linux_domain_same(adopter,child) || (child->p_lflag & P_LTRACED) ||')
    edits[path]=(before,text.encode())
    return edits
