/* Included in kern_descrip.c; publication is bounded to single-thread procs.
 * Linux thread creation remains unsupported, but check rather than assume it. */
#include <sys/user.h>
static bool rd_launch_single_thread(struct proc *p)
{
    unsigned count=0;struct uthread *uth;proc_lock(p);
    TAILQ_FOREACH(uth,&p->p_uthlist,uu_list)count++;
    proc_unlock(p);return count==1;
}
int rd_linux_pipe2(struct proc *p,user_addr_t address,uint32_t flags)
{
    if(flags&~0x80800U)return EINVAL;
    if(!rd_launch_single_thread(p))return ENOTSUP;
    int32_t fds[2]={-1,-1};struct pipe_args args={};int error=pipe(p,&args,fds);
    if(error)return error;
    for(unsigned i=0;i<2&&!error;i++){
        int32_t ignored=0;struct fcntl_args cmd={.fd=fds[i]};
        if(flags&0x800){cmd.cmd=F_SETFL;cmd.arg=O_NONBLOCK;error=sys_fcntl(p,&cmd,&ignored);}
        if(!error&&(flags&0x80000)){cmd.cmd=F_SETFD;cmd.arg=FD_CLOEXEC;error=sys_fcntl(p,&cmd,&ignored);}
    }
    if(!error)error=copyout(fds,address,sizeof(fds));
    if(error)for(unsigned i=0;i<2;i++){struct close_args close_args={.fd=fds[i]};int32_t ignored;sys_close(p,&close_args,&ignored);}
    return error;
}
int rd_linux_dup3(struct proc *p,int from,int to,uint32_t flags,int32_t *result)
{
    if(from==to||(flags&~0x80000U))return EINVAL;
    if(!rd_launch_single_thread(p))return ENOTSUP;
    if(rd_linux_epoll_close_alias(p,to))return ENOTSUP;
    struct dup2_args args={.from=from,.to=to};int error=sys_dup2(p,&args,result);
    if(!error&&(flags&0x80000)){struct fcntl_args cmd={.fd=to,.cmd=F_SETFD,.arg=FD_CLOEXEC};
        int32_t ignored;error=sys_fcntl(p,&cmd,&ignored);}
    return error;
}
