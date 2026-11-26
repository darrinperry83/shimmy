/* Service launch controls retain native process/session/descriptor ownership. */
int rd_linux_pipe2(struct proc *,user_addr_t,uint32_t);
int rd_linux_dup3(struct proc *,int,int,uint32_t,int32_t *);
int rd_linux_prlimit(struct proc *,int,unsigned,user_addr_t,user_addr_t);
static int64_t rd_linux_launch(struct proc *p,arm_saved_state_t *state,uint64_t nr)
{
    uint64_t a0=get_saved_state_reg(state,0),a1=get_saved_state_reg(state,1);
    uint64_t a2=get_saved_state_reg(state,2),a3=get_saved_state_reg(state,3);
    int error=0;int32_t result=0;
    if(nr==59)error=rd_linux_pipe2(p,a0,(uint32_t)a1);
    else if(nr==24)error=rd_linux_dup3(p,(int)a0,(int)a1,(uint32_t)a2,&result);
    else if(nr==23){struct dup_args args={.fd=(int)a0};error=sys_dup(p,&args,&result);}
    else if(nr==25){struct fcntl_args args={.fd=(int)a0};
        if(a1==1030){args.cmd=F_DUPFD_CLOEXEC;args.arg=a2;}
        else{if(rd_linux_path_fd(p,(int)a0))return -9;
            if(a2&~0x20c03ULL)return -95;args.cmd=F_SETFL;
            args.arg=((a2&0x400)?O_APPEND:0)|((a2&0x800)?O_NONBLOCK:0);}
        error=sys_fcntl(p,&args,&result);
    }else if(nr==154){struct setpgid_args args={.pid=(int)a0,.pgid=(int)a1};error=setpgid(p,&args,&result);}
    else if(nr==155){struct getpgid_args args={.pid=(int)a0};error=getpgid(p,&args,&result);}
    else if(nr==156){struct getsid_args args={.pid=(int)a0};error=getsid(p,&args,&result);}
    else if(nr==157){struct setsid_args args={};error=setsid(p,&args,&result);}
    else if(nr==166){struct umask_args args={.newmask=(int)a0};error=umask(p,&args,&result);}
    else if(nr==163){if(!a1)return -14;error=rd_linux_prlimit(p,0,(unsigned)a0,0,a1);}
    else if(nr==164){if(!a1)return -14;error=rd_linux_prlimit(p,0,(unsigned)a0,a1,0);}
    else if(nr==261)error=rd_linux_prlimit(p,(int)a0,(unsigned)a1,a2,a3);
    else if(nr==260){
        if(a2&~1ULL)return -95; /* Exited children, optional WNOHANG. */
        struct wait4_nocancel_args args={.pid=(int)a0,.status=a1,.options=(a2&1)?WNOHANG:0,.rusage=a3};
        error=wait4_nocancel(p,&args,&result);
    }else return -38;
    if(error==ERESTART)error=EINTR;
    return error?-rd_linux_errno(error):result;
}
