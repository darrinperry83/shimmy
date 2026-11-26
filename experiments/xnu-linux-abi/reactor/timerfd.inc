/* Included after the eventfd backend in kern_descrip.c. */
#include <kern/thread_call.h>
struct rd_timespec {int64_t sec,nsec;};
struct rd_itimer {struct rd_timespec interval,value;};
struct rd_timerfd {
    struct rd_eventfd counter; /* common readiness/counter backend, offset zero */
    lck_mtx_t command;thread_call_t call;uint64_t deadline,interval;
    int clock;bool armed,closing;
};
static uint64_t rd_timer_now(struct rd_timerfd *t)
{uint64_t ns;absolutetime_to_nanoseconds(t->clock==7?mach_continuous_time():mach_absolute_time(),&ns);return ns;}
static void rd_timer_schedule(struct rd_timerfd *t)
{
    uint64_t ticks;nanoseconds_to_absolutetime(t->deadline,&ticks);
    thread_call_enter_delayed_with_leeway(t->call,NULL,ticks,0,
        THREAD_CALL_DELAY_USER_NORMAL|(t->clock==7?THREAD_CALL_CONTINUOUS:0));
}
static void rd_timer_refresh(struct rd_timerfd *t,uint64_t now)
{
    if(!t->armed||now<t->deadline)return;
    uint64_t count=1;if(t->interval)count+=(now-t->deadline)/t->interval;
    t->counter.count=UINT64_MAX-t->counter.count<count?UINT64_MAX:t->counter.count+count;
    if(t->interval&&count<=(UINT64_MAX-t->deadline)/t->interval)t->deadline+=count*t->interval;
    else t->armed=false;
    rd_eventfd_wake(&t->counter);
}
static void rd_timer_fire(thread_call_param_t first,thread_call_param_t second)
{
    (void)second;struct rd_timerfd *t=first;lck_mtx_lock(&t->counter.lock);
    if(!t->closing&&t->armed){rd_timer_refresh(t,rd_timer_now(t));if(t->armed)rd_timer_schedule(t);}
    lck_mtx_unlock(&t->counter.lock);
}
static int rd_timer_badwrite(struct fileproc *fp,struct uio *uio,int flags,vfs_context_t ctx)
{(void)fp;(void)uio;(void)flags;(void)ctx;return EINVAL;}
static int rd_timer_close(struct fileglob *fg,vfs_context_t ctx)
{
    (void)ctx;struct rd_timerfd *t=fg_get_data(fg);
    lck_mtx_lock(&t->counter.lock);t->closing=true;t->armed=false;lck_mtx_unlock(&t->counter.lock);
    thread_call_cancel_wait(t->call);thread_call_free(t->call);
    lck_mtx_lock(&t->counter.lock);selthreadclear(&t->counter.select);lck_mtx_unlock(&t->counter.lock);
    lck_mtx_destroy(&t->counter.lock,&rd_eventfd_locks);lck_mtx_destroy(&t->command,&rd_eventfd_locks);
    kfree_type(struct rd_timerfd,t);return 0;
}
static int rd_timer_kqfilter(struct fileproc *fp,struct knote *kn,struct kevent_qos_s *kev)
{if(kn->kn_filter!=EVFILT_READ){knote_set_error(kn,ENOTSUP);return 0;}return rd_eventfd_kqfilter(fp,kn,kev);}
static int rd_timer_select(struct fileproc *fp,int which,void *wql,vfs_context_t ctx)
{return which==FREAD?rd_eventfd_select(fp,which,wql,ctx):0;}
static const struct fileops rd_timer_ops={.fo_type=DTYPE_RDXNU_TIMERFD,
    .fo_read=rd_eventfd_read,.fo_write=rd_timer_badwrite,.fo_ioctl=rd_eventfd_ioctl,
    .fo_select=rd_timer_select,.fo_close=rd_timer_close,.fo_drain=rd_eventfd_drain,.fo_kqfilter=rd_timer_kqfilter};
int rd_linux_timer_create(struct proc *p,int clock,uint32_t flags,int32_t *result)
{
    if(flags&~0x80800U)return EINVAL;
    if(clock!=1&&clock!=7)return clock==0||clock==8||clock==9?ENOTSUP:EINVAL;
    struct rd_timerfd *t=kalloc_type(struct rd_timerfd,Z_WAITOK|Z_ZERO);if(!t)return ENOMEM;
    lck_mtx_init(&t->counter.lock,&rd_eventfd_locks,LCK_ATTR_NULL);
    lck_mtx_init(&t->command,&rd_eventfd_locks,LCK_ATTR_NULL);t->clock=clock;
    t->call=thread_call_allocate(rd_timer_fire,t);
    if(!t->call){lck_mtx_destroy(&t->counter.lock,&rd_eventfd_locks);lck_mtx_destroy(&t->command,&rd_eventfd_locks);kfree_type(struct rd_timerfd,t);return ENOMEM;}
    struct fileproc *fp;int fd;int error=falloc_withinit(p,current_cached_proc_cred(p),vfs_context_current(),&fp,&fd,NULL,NULL);
    if(error){thread_call_free(t->call);lck_mtx_destroy(&t->counter.lock,&rd_eventfd_locks);lck_mtx_destroy(&t->command,&rd_eventfd_locks);kfree_type(struct rd_timerfd,t);return error;}
    fp->fp_glob->fg_flag=FREAD|FWRITE|((flags&0x800)?FNONBLOCK:0);fp->fp_glob->fg_ops=&rd_timer_ops;fp_set_data(fp,t);
    if(flags&0x80000)fp->fp_flags|=FP_CLOEXEC;
    proc_fdlock(p);procfdtbl_releasefd(p,fd,NULL);fp_drop(p,fd,fp,1);proc_fdunlock(p);*result=fd;return 0;
}
static int rd_timer_ns(struct rd_timespec x,uint64_t *out)
{
    if(x.sec<0||x.nsec<0||x.nsec>=1000000000)return EINVAL;
    if((uint64_t)x.sec>(UINT64_MAX-(uint64_t)x.nsec)/1000000000)return EINVAL;
    *out=(uint64_t)x.sec*1000000000+x.nsec;return 0;
}
static struct rd_timespec rd_timer_time(uint64_t ns)
{return (struct rd_timespec){.sec=(int64_t)(ns/1000000000),.nsec=(int64_t)(ns%1000000000)};}
int rd_linux_timer_control(struct proc *p,int fd,bool set,uint32_t flags,user_addr_t address,user_addr_t old_address)
{
    if(set&&(flags&~1U))return flags&~3U?EINVAL:ENOTSUP;
    struct rd_itimer requested={0},prior={0};uint64_t value=0,interval=0;
    if(set){if(copyin(address,&requested,sizeof(requested)))return EFAULT;
        int error=rd_timer_ns(requested.value,&value);if(!error)error=rd_timer_ns(requested.interval,&interval);if(error)return error;}
    struct fileproc *fp;int error=fp_lookup(p,fd,&fp,0);if(error)return error;
    if(fp->fp_glob->fg_ops!=&rd_timer_ops){fp_drop(p,fd,fp,0);return EINVAL;}
    struct rd_timerfd *t=fp_get_data(fp);lck_mtx_lock(&t->command);lck_mtx_lock(&t->counter.lock);
    uint64_t now=rd_timer_now(t);rd_timer_refresh(t,now);
    prior.interval=rd_timer_time(t->interval);prior.value=rd_timer_time(t->armed?t->deadline-now:0);
    if(set&&value&&!(flags&1)&&value>UINT64_MAX-now){error=EINVAL;lck_mtx_unlock(&t->counter.lock);goto done;}
    if(set){t->armed=false;lck_mtx_unlock(&t->counter.lock);thread_call_cancel_wait(t->call);lck_mtx_lock(&t->counter.lock);
        t->interval=interval;t->deadline=(flags&1)?value:rd_timer_now(t)+value;t->counter.count=0;t->armed=value!=0;
        if(t->armed){rd_timer_refresh(t,rd_timer_now(t));if(t->armed)rd_timer_schedule(t);}}
    lck_mtx_unlock(&t->counter.lock);
    if(set){if(old_address)error=copyout(&prior,old_address,sizeof(prior));}
    else error=copyout(&prior,address,sizeof(prior));
done:lck_mtx_unlock(&t->command);fp_drop(p,fd,fp,0);return error;
}
bool rd_linux_reactor_getfl(struct proc *p,int fd,int64_t *value)
{
    struct fileproc *fp;if(fp_lookup(p,fd,&fp,0))return false;
    file_type_t type=FILEGLOB_DTYPE(fp->fp_glob);
    bool match=type==DTYPE_RDXNU_EPOLL||type==DTYPE_RDXNU_TIMERFD||type==DTYPE_RDXNU_SIGNALFD;
    if(match)*value=2|((fp->fp_glob->fg_flag&FNONBLOCK)?0x800:0);
    fp_drop(p,fd,fp,0);return match;
}
