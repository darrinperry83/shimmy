/* Process-private Linux futexes. Native msleep atomically releases the mutex
 * after asserting the wait; comparison and wake selection share that mutex. */
static LCK_GRP_DECLARE(rd_futex_group,"rd_linux_futex");
static LCK_MTX_DECLARE(rd_futex_lock,&rd_futex_group);
struct rd_futex_waiter {
    struct rd_futex_waiter *next;struct proc *owner;user_addr_t address;bool notified;
};
static struct rd_futex_waiter *rd_futex_waiters;
int rd_linux_futex(struct proc *p,user_addr_t address,uint32_t op,int32_t value,
                   user_addr_t timeout,int32_t *result)
{
    if(address&3)return EINVAL;
    if(op!=128&&op!=129)return ENOTSUP; /* WAIT/WAKE_PRIVATE; no shared/PI/bitset. */
    if(op==129){
        /* Legacy FUTEX_WAKE selects the first waiter even for counts <= 0. */
        int limit=value>0?value:1,count=0;lck_mtx_lock(&rd_futex_lock);
        for(struct rd_futex_waiter *w=rd_futex_waiters;w&&count<limit;w=w->next)
            if(w->owner==p&&w->address==address&&!w->notified&&thread_wakeup_one(w)==KERN_SUCCESS){
                w->notified=true;count++;
            }
        lck_mtx_unlock(&rd_futex_lock);*result=count;return 0;
    }
    struct {int64_t sec,nsec;} linux_time={0};struct timespec ts={0};
    if(timeout){
        if(copyin(timeout,&linux_time,sizeof(linux_time)))return EFAULT;
        if(linux_time.sec<0||linux_time.nsec<0||linux_time.nsec>=NSEC_PER_SEC)return EINVAL;
        /* Bound conversion before native msleep's nanosecond multiplication. */
        if(linux_time.sec>INT_MAX)return ENOTSUP;
        ts.tv_sec=linux_time.sec;ts.tv_nsec=linux_time.nsec;
    }
    int error=0;uint32_t observed;lck_mtx_lock(&rd_futex_lock);
    if(copyin(address,&observed,sizeof(observed))){error=EFAULT;goto out;}
    if(observed!=(uint32_t)value){error=EAGAIN;goto out;}
    if(timeout&&!ts.tv_sec&&!ts.tv_nsec){error=ETIMEDOUT;goto out;}
    struct rd_futex_waiter waiter={.next=rd_futex_waiters,.owner=p,.address=address};
    rd_futex_waiters=&waiter;
    do{error=msleep(&waiter,&rd_futex_lock,PWAIT|PCATCH,"linux_futex",timeout?&ts:NULL);}
    while(!error&&!waiter.notified);
    struct rd_futex_waiter **link=&rd_futex_waiters;
    while(*link&&*link!=&waiter)link=&(*link)->next;
    assert(*link==&waiter);*link=waiter.next;
    if(waiter.notified)error=0;
    else if(error==EWOULDBLOCK)error=ETIMEDOUT;
    else if(error==ERESTART)error=EINTR;
out:lck_mtx_unlock(&rd_futex_lock);*result=0;return error;
}
