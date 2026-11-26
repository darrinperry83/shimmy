/* Bounded process lifecycle: XNU owns fork/COW, identities, signals and reaping. */
#include <sys/wait.h>
void rd_linux_fork_child(struct proc *parent,struct proc *child,thread_t thread)
{
    struct uthread *source=get_bsdthread_info(current_thread());
    if(!source->rd_linux_enabled)return;
    os_atomic_or(&child->p_ladvflag,P_RDXNU_LINUX,relaxed);
    struct uthread *target=get_bsdthread_info(thread);
    target->rd_linux_tls=source->rd_linux_tls;
    target->rd_linux_mask=source->rd_linux_mask;
    target->rd_linux_clear_tid=0;target->rd_linux_enabled=1;
    arm_saved_state_t *state=(arm_saved_state_t *)(void *)get_user_regs(thread);
    arm_saved_state_t *caller=(arm_saved_state_t *)(void *)get_user_regs(current_thread());
    set_user_saved_state_reg(state,0,0);
    set_user_saved_state_reg(state,1,get_saved_state_reg(caller,1));
    (void)parent;
}
static int64_t rd_linux_process(struct proc *p,arm_saved_state_t *state,uint64_t nr)
{
    uint64_t a0=get_saved_state_reg(state,0),a1=get_saved_state_reg(state,1);
    uint64_t a2=get_saved_state_reg(state,2),a3=get_saved_state_reg(state,3);
    uint64_t a4=get_saved_state_reg(state,4);
    int error=0;int32_t result[2]={0,0};
    if(nr==173)return proc_ppid(p);
    if(nr==220){
        /* Fork only. Shared-VM/vfork/thread/tid-pointer modes require separate gates. */
        if(a0!=17 || a1 || a2 || a3 || a4)return -95;
        struct fork_args args={};error=fork(p,&args,result);
        return error ? -rd_linux_errno(error) : result[0];
    }
    if(nr==221){
        struct execve_args args={};args.fname=a0;args.argp=a1;args.envp=a2;
        error=execve(p,&args,result);
    }else if(nr==129){
        if((int)a0<=0)return -95;
        int signal=(int)a1;
        if(signal<0 || signal>64)return -22;
        int native=signal ? rd_linux_signal_native(signal) : 0;
        if(signal && !native)return -95;
        struct kill_args args={};args.pid=(int)a0;args.signum=native;
        error=kill(p,&args,result);
    }else if(nr==95){
        if(a3 & ~0x100000fULL)return -22;
        if(!(a3&0xe))return -22;
        if(a0>3)return -22;
        if(a0==1 && ((int)a1<=0 || a1>INT32_MAX))return -22;
        if(a0>1 || (a3&0xa) || a4)return -95;
        struct waitid_nocancel_args args={};
        args.idtype=a0==0 ? P_ALL : P_PID;args.id=(id_t)a1;
        args.infop=a2;args.options=WEXITED;
        if(a3&1)args.options|=WNOHANG;
        if(a3&0x1000000)args.options|=WNOWAIT;
        error=waitid_nocancel(p,&args,result);
    }else return -38;
    return error ? -rd_linux_errno(error) : 0;
}
