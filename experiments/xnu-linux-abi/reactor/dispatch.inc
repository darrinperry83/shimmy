/* Batched native event-loop interfaces. AArch64 epoll_event is 16 bytes. */
int rd_linux_epoll_create(struct proc *,uint32_t,int32_t *);
int rd_linux_epoll_ctl(struct proc *,int,int,int,user_addr_t);
int rd_linux_epoll_wait(struct proc *,int,user_addr_t,int,int,int32_t *);
bool rd_linux_epoll_close_alias(struct proc *,int);
bool rd_linux_epoll_exec_guard(struct proc *);
bool rd_linux_reactor_getfl(struct proc *,int,int64_t *);
int rd_linux_timer_create(struct proc *,int,uint32_t,int32_t *);
int rd_linux_timer_control(struct proc *,int,bool,uint32_t,user_addr_t,user_addr_t);
int rd_linux_signalfd(struct proc *,int,user_addr_t,uint64_t,uint32_t,int32_t *);
int rd_linux_futex(struct proc *,user_addr_t,uint32_t,int32_t,user_addr_t,int32_t *);
static int64_t rd_linux_reactor(struct proc *p,arm_saved_state_t *state,uint64_t nr)
{
    uint64_t a0=get_saved_state_reg(state,0),a1=get_saved_state_reg(state,1),a2=get_saved_state_reg(state,2);
    uint64_t a3=get_saved_state_reg(state,3),a4=get_saved_state_reg(state,4);int error;int32_t result=0;
    if(nr==98)error=rd_linux_futex(p,a0,(uint32_t)a1,(int32_t)a2,a3,&result);
    else if(nr==20)error=rd_linux_epoll_create(p,(uint32_t)a0,&result);
    else if(nr==21)error=rd_linux_epoll_ctl(p,(int)a0,(int)a1,(int)a2,a3);
    else if(nr==22){if(a4)return -95;error=rd_linux_epoll_wait(p,(int)a0,a1,(int)a2,(int)a3,&result);}
    else if(nr==85)error=rd_linux_timer_create(p,(int)a0,(uint32_t)a1,&result);
    else if(nr==86)error=rd_linux_timer_control(p,(int)a0,true,(uint32_t)a1,a2,a3);
    else if(nr==87)error=rd_linux_timer_control(p,(int)a0,false,0,a1,0);
    else error=rd_linux_signalfd(p,(int)a0,a1,a2,(uint32_t)a3,&result);
    if(error==ERESTART)error=EINTR;
    if(error==ETIMEDOUT)return -110;
    return error?-rd_linux_errno(error):result;
}
