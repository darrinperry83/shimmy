/* eventfd2 and bounded ppoll; native fileglob/kqueue own state and wakeups. */
bool rd_linux_eventfd_getfl(struct proc *,int,int64_t *);
int rd_linux_eventfd(struct proc *,uint32_t,uint32_t,int32_t *);
static int64_t rd_linux_events(struct proc *p,arm_saved_state_t *state,uint64_t nr)
{
    uint64_t a0=get_saved_state_reg(state,0),a1=get_saved_state_reg(state,1);
    uint64_t a2=get_saved_state_reg(state,2),a3=get_saved_state_reg(state,3);
    int32_t result=0;int error;
    if(nr==19){error=rd_linux_eventfd(p,(uint32_t)a0,(uint32_t)a1,&result);}
    else{
        if(a3)return -95; /* Atomic temporary signal masks need a separate gate. */
        if(a1>128)return -22;
        struct {int64_t sec,nsec;} timeout={0,0};int milliseconds=-1;
        uint64_t requested=0,start=0;
        if(a2){
            if(copyin(a2,&timeout,sizeof(timeout)))return -14;
            if(timeout.sec<0||timeout.nsec<0||timeout.nsec>=1000000000)return -22;
            if(timeout.sec>INT32_MAX/1000)return -95;
            requested=timeout.sec*1000000000ULL+timeout.nsec;
            uint64_t ms=(requested+999999)/1000000;
            if(ms>INT32_MAX)return -95;
            milliseconds=(int)ms;absolutetime_to_nanoseconds(mach_absolute_time(),&start);
        }
        struct poll_nocancel_args args={};args.fds=a0;args.nfds=(unsigned)a1;args.timeout=milliseconds;
        error=poll_nocancel(p,&args,&result);
        if(a2 && (!error||error==EINTR||error==ERESTART)){
            uint64_t end=0;absolutetime_to_nanoseconds(mach_absolute_time(),&end);
            uint64_t left=end-start<requested?requested-(end-start):0;
            timeout.sec=left/1000000000;timeout.nsec=left%1000000000;
            if(copyout(&timeout,a2,sizeof(timeout)))error=EFAULT;
        }
    }
    if(error==ERESTART)error=EINTR;
    return error?-rd_linux_errno(error):result;
}
