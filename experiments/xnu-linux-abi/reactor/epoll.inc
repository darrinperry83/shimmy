/* Included at the end of kern_event.c: native kqueue owns subscriptions/waits. */
#define RD_EP_SLOTS 32
struct rd_ep_event { uint32_t events,pad;uint64_t data; };
struct rd_ep_slot { int fd;bool used,armed;uint32_t events,generation;uint64_t data; };
struct rd_epoll {
    struct rd_epoll *next;struct proc *owner;struct kqueue *queue;
    lck_mtx_t operation;unsigned cursor;uint32_t serial;
    struct rd_ep_slot slots[RD_EP_SLOTS];
};
static LCK_GRP_DECLARE(rd_ep_group,"rd_linux_epoll");
static LCK_MTX_DECLARE(rd_ep_lifetime,&rd_ep_group);
static LCK_SPIN_DECLARE(rd_ep_registry_lock,&rd_ep_group);
static struct rd_epoll *rd_ep_registry;
static int rd_ep_badread(struct fileproc *fp,struct uio *uio,int flags,vfs_context_t ctx)
{(void)fp;(void)uio;(void)flags;(void)ctx;return EINVAL;}
static int rd_ep_ioctl(struct fileproc *fp,u_long cmd,caddr_t data,vfs_context_t ctx)
{(void)fp;(void)cmd;(void)data;(void)ctx;return ENOTTY;}
static int rd_ep_select(struct fileproc *fp,int which,void *wql,vfs_context_t ctx)
{(void)fp;(void)which;(void)wql;(void)ctx;return 0;}
static int rd_ep_kqfilter(struct fileproc *fp,struct knote *kn,struct kevent_qos_s *kev)
{(void)fp;(void)kev;knote_set_error(kn,ENOTSUP);return 0;}
static int rd_ep_drain(struct fileproc *fp,vfs_context_t ctx)
{
    (void)ctx;struct rd_epoll *e=fp_get_data(fp);
    lck_spin_lock(&rd_ep_registry_lock);
    if(e->queue){struct kqfile *q=(struct kqfile *)e->queue;
        kqlock(q);q->kqf_state|=KQ_DRAIN;kqfile_wakeup(q,NOTE_REVOKE,THREAD_RESTART);kqunlock(q);}
    lck_spin_unlock(&rd_ep_registry_lock);return 0;
}
static int rd_ep_close(struct fileglob *fg,vfs_context_t ctx)
{
    (void)ctx;struct rd_epoll *e=fg_get_data(fg);struct kqueue *q;
    lck_mtx_lock(&rd_ep_lifetime);lck_spin_lock(&rd_ep_registry_lock);
    struct rd_epoll **link=&rd_ep_registry;while(*link&&*link!=e)link=&(*link)->next;
    if(*link)*link=e->next;q=e->queue;e->queue=NULL;e->owner=NULL;
    lck_spin_unlock(&rd_ep_registry_lock);
    if(q)kqueue_dealloc(q);
    lck_mtx_unlock(&rd_ep_lifetime);
    lck_mtx_destroy(&e->operation,&rd_ep_group);kfree_type(struct rd_epoll,e);return 0;
}
static const struct fileops rd_ep_ops={.fo_type=DTYPE_RDXNU_EPOLL,
    .fo_read=rd_ep_badread,.fo_write=rd_ep_badread,.fo_ioctl=rd_ep_ioctl,
    .fo_select=rd_ep_select,.fo_close=rd_ep_close,.fo_drain=rd_ep_drain,.fo_kqfilter=rd_ep_kqfilter};
/* Called before native fd-table teardown. Child copies can retain the fileglob,
 * but may not retain a dangling kqueue owner or pin native process references. */
void rd_linux_epoll_owner_cleanup(struct proc *p)
{
    lck_mtx_lock(&rd_ep_lifetime);
    for(;;){
        struct kqueue *q=NULL;lck_spin_lock(&rd_ep_registry_lock);
        for(struct rd_epoll *e=rd_ep_registry;e;e=e->next)if(e->owner==p){
            q=e->queue;e->queue=NULL;e->owner=NULL;break;
        }
        lck_spin_unlock(&rd_ep_registry_lock);if(!q)break;kqueue_dealloc(q);
    }
    lck_mtx_unlock(&rd_ep_lifetime);
}
/* Native fdclose already tears down its knotes. Retire the matching interest
 * before the fd number can be reused; no weak fileglob pointer is retained. */
void rd_linux_epoll_fdclose(struct proc *p,int fd)
{
    lck_spin_lock(&rd_ep_registry_lock);
    for(struct rd_epoll *e=rd_ep_registry;e;e=e->next)if(e->owner==p)
        for(unsigned i=0;i<RD_EP_SLOTS;i++)if(e->slots[i].used&&e->slots[i].fd==fd)e->slots[i].used=false;
    lck_spin_unlock(&rd_ep_registry_lock);
}
bool rd_linux_epoll_close_alias(struct proc *p,int fd)
{
    struct fileproc *fp;if(fp_lookup(p,fd,&fp,0))return false;
    bool shared=os_ref_get_count_raw(&fp->fp_glob->fg_count)>1,watched=false;
    if(shared){lck_spin_lock(&rd_ep_registry_lock);
        for(struct rd_epoll *e=rd_ep_registry;e;e=e->next)if(e->owner==p)
            for(unsigned i=0;i<RD_EP_SLOTS;i++)if(e->slots[i].used&&e->slots[i].fd==fd)watched=true;
        lck_spin_unlock(&rd_ep_registry_lock);}
    fp_drop(p,fd,fp,0);return watched;
}
bool rd_linux_epoll_exec_guard(struct proc *p)
{
    bool blocked=false;proc_fdlock(p);
    for(int fd=0;fd<p->p_fd.fd_nfiles;fd++){
        struct fileproc *fp=fp_get_noref_locked(p,fd);
        if(fp&&fp->fp_glob->fg_ops==&rd_ep_ops&&!(fp->fp_flags&FP_CLOEXEC)){
            struct rd_epoll *e=fp_get_data(fp);if(e->owner==p)blocked=true;
        }
    }
    proc_fdunlock(p);return blocked;
}
int rd_linux_epoll_create(struct proc *p,uint32_t flags,int32_t *result)
{
    if(flags&~0x80000U)return EINVAL;
    struct rd_epoll *e=kalloc_type(struct rd_epoll,Z_WAITOK|Z_ZERO);if(!e)return ENOMEM;
    e->queue=kqueue_alloc(p);if(!e->queue){kfree_type(struct rd_epoll,e);return ENOMEM;}
    e->owner=p;lck_mtx_init(&e->operation,&rd_ep_group,LCK_ATTR_NULL);
    struct fileproc *fp;int fd;int error=falloc_withinit(p,current_cached_proc_cred(p),vfs_context_current(),&fp,&fd,NULL,NULL);
    if(error){kqueue_dealloc(e->queue);lck_mtx_destroy(&e->operation,&rd_ep_group);kfree_type(struct rd_epoll,e);return error;}
    fp->fp_glob->fg_flag=FREAD|FWRITE;fp->fp_glob->fg_ops=&rd_ep_ops;fp_set_data(fp,e);
    if(flags&0x80000)fp->fp_flags|=FP_CLOEXEC;
    lck_spin_lock(&rd_ep_registry_lock);e->next=rd_ep_registry;rd_ep_registry=e;lck_spin_unlock(&rd_ep_registry_lock);
    proc_fdlock(p);procfdtbl_releasefd(p,fd,NULL);fp_drop(p,fd,fp,1);proc_fdunlock(p);*result=fd;return 0;
}
static int rd_ep_change(struct rd_epoll *e,int fd,int filter,int flags,uint64_t token)
{
    struct kevent_qos_s kev={.ident=(uint64_t)fd,.filter=(int16_t)filter,.flags=(uint16_t)flags,.udata=token};
    int rc=kevent_register(e->queue,&kev,NULL);assert(!(rc&FILTER_REGISTER_WAIT));
    return kev.flags&EV_ERROR?(int)kev.data:0;
}
int rd_linux_epoll_ctl(struct proc *p,int epfd,int op,int fd,user_addr_t address)
{
    if(op<1||op>3)return EINVAL;
    struct fileproc *fp;int error=fp_lookup(p,epfd,&fp,0);if(error)return error;
    if(fp->fp_glob->fg_ops!=&rd_ep_ops){fp_drop(p,epfd,fp,0);return EINVAL;}
    struct rd_epoll *e=fp_get_data(fp);lck_mtx_lock(&e->operation);
    if(e->owner!=p||!e->queue){error=ENOTSUP;goto out;}
    struct fileproc *target;error=fp_lookup(p,fd,&target,0);if(error)goto out;
    if(target->fp_glob==fp->fp_glob)error=EINVAL;
    else if(FILEGLOB_DTYPE(target->fp_glob)==DTYPE_VNODE)error=EPERM;
    else if(FILEGLOB_DTYPE(target->fp_glob)!=DTYPE_PIPE&&FILEGLOB_DTYPE(target->fp_glob)!=DTYPE_RDXNU_EVENTFD&&
            FILEGLOB_DTYPE(target->fp_glob)!=DTYPE_RDXNU_TIMERFD&&FILEGLOB_DTYPE(target->fp_glob)!=DTYPE_RDXNU_SIGNALFD)error=ENOTSUP;
    fp_drop(p,fd,target,0);if(error)goto out;
    struct rd_ep_event event={0};if(op!=2&&copyin(address,&event,sizeof(event))){error=EFAULT;goto out;}
    if(op!=2&&((event.events&~0x4000001dU)||!(event.events&5))){error=ENOTSUP;goto out;}
    int slot=-1,free_slot=-1;struct rd_ep_slot old={0};
    lck_spin_lock(&rd_ep_registry_lock);
    for(unsigned i=0;i<RD_EP_SLOTS;i++){
        if(e->slots[i].used&&e->slots[i].fd==fd)slot=(int)i;
        if(!e->slots[i].used&&free_slot<0)free_slot=(int)i;
    }
    if(slot>=0)old=e->slots[slot];lck_spin_unlock(&rd_ep_registry_lock);
    if(op==1&&slot>=0){error=EEXIST;goto out;}
    if(op!=1&&slot<0){error=ENOENT;goto out;}
    if(op==1)slot=free_slot;if(slot<0){error=ENOSPC;goto out;}
    uint32_t generation=++e->serial;if(!generation)generation=++e->serial;
    uint64_t token=((uint64_t)generation<<32)|(unsigned)(slot+1);
    uint32_t wanted=op==2?0:event.events;
    if(wanted&1)error=rd_ep_change(e,fd,EVFILT_READ,EV_ADD|EV_ENABLE,token);
    if(!error&&(wanted&4))error=rd_ep_change(e,fd,EVFILT_WRITE,EV_ADD|EV_ENABLE,token);
    if(error){
        if(!old.used){(void)rd_ep_change(e,fd,EVFILT_READ,EV_DELETE,0);(void)rd_ep_change(e,fd,EVFILT_WRITE,EV_DELETE,0);}
        else{uint64_t prior=((uint64_t)old.generation<<32)|(unsigned)(slot+1);
            if(old.events&1)(void)rd_ep_change(e,fd,EVFILT_READ,EV_ADD|(old.armed?EV_ENABLE:EV_DISABLE),prior);else(void)rd_ep_change(e,fd,EVFILT_READ,EV_DELETE,0);
            if(old.events&4)(void)rd_ep_change(e,fd,EVFILT_WRITE,EV_ADD|(old.armed?EV_ENABLE:EV_DISABLE),prior);else(void)rd_ep_change(e,fd,EVFILT_WRITE,EV_DELETE,0);}
        goto out;
    }
    if(!(wanted&1)&&old.used&&(old.events&1))(void)rd_ep_change(e,fd,EVFILT_READ,EV_DELETE,0);
    if(!(wanted&4)&&old.used&&(old.events&4))(void)rd_ep_change(e,fd,EVFILT_WRITE,EV_DELETE,0);
    lck_spin_lock(&rd_ep_registry_lock);
    e->slots[slot]=(struct rd_ep_slot){.fd=fd,.used=op!=2,.armed=op!=2,.events=wanted,.generation=generation,.data=event.data};
    lck_spin_unlock(&rd_ep_registry_lock);
out:lck_mtx_unlock(&e->operation);fp_drop(p,epfd,fp,0);return error;
}
struct rd_ep_scan {struct rd_ep_slot slots[RD_EP_SLOTS];uint32_t ready[RD_EP_SLOTS];};
static int rd_ep_callback(struct kevent_qos_s *kev,kevent_ctx_t ctx)
{
    struct rd_ep_scan *scan=(void *)ctx->kec_poll_fds;unsigned index=(unsigned)kev->udata;
    if(!index||index>RD_EP_SLOTS)return 0;index--;
    struct rd_ep_slot *s=&scan->slots[index];if(!s->used||!s->armed||s->generation!=(uint32_t)(kev->udata>>32))return 0;
    uint32_t bits=0;
    if(kev->flags&EV_ERROR)bits|=8;
    if(kev->flags&EV_EOF){bits|=kev->filter==EVFILT_WRITE?8:16;
        if(kev->filter==EVFILT_READ&&kev->data>0)bits|=1;
    }else bits|=kev->filter==EVFILT_READ?1:4;
    bits&=s->events|24;
    if(bits&&!scan->ready[index])ctx->kec_process_noutputs++;
    scan->ready[index]|=bits;return 0;
}
int rd_linux_epoll_wait(struct proc *p,int fd,user_addr_t address,int maximum,int timeout,int32_t *result)
{
    if(maximum<=0)return EINVAL;if(maximum>RD_EP_SLOTS)return ENOTSUP;
    struct fileproc *fp;int error=fp_lookup(p,fd,&fp,0);if(error)return error;
    if(fp->fp_glob->fg_ops!=&rd_ep_ops){fp_drop(p,fd,fp,0);return EINVAL;}
    struct rd_epoll *e=fp_get_data(fp);lck_mtx_lock(&e->operation);
    if(e->owner!=p||!e->queue){error=ENOTSUP;goto out;}
    struct rd_ep_scan scan={0};struct rd_ep_event events[RD_EP_SLOTS]={0};unsigned selected[RD_EP_SLOTS]={0};
    lck_spin_lock(&rd_ep_registry_lock);memcpy(scan.slots,e->slots,sizeof(scan.slots));lck_spin_unlock(&rd_ep_registry_lock);
    kevent_ctx_t ctx=kevent_get_context(current_thread());
    *ctx=(struct kevent_ctx_s){.kec_process_flags=KEVENT_FLAG_POLL,.kec_poll_fds=(void *)&scan};
    if(timeout==0)ctx->kec_process_flags|=KEVENT_FLAG_IMMEDIATE;
    else if(timeout>0)clock_interval_to_deadline(timeout,NSEC_PER_MSEC,&ctx->kec_deadline);
    error=kqueue_scan(e->queue,ctx->kec_process_flags,ctx,rd_ep_callback);
    if(error==ERESTART)error=EINTR;
    if(!error){unsigned count=0,next=e->cursor;
        for(unsigned j=0;j<RD_EP_SLOTS&&count<(unsigned)maximum;j++){
            unsigned i=(e->cursor+j)%RD_EP_SLOTS;if(!scan.ready[i])continue;
            selected[count]=i;events[count++]=(struct rd_ep_event){.events=scan.ready[i],.data=scan.slots[i].data};next=(i+1)%RD_EP_SLOTS;
        }
        if(count)error=copyout(events,address,count*sizeof(events[0]));
        if(!error){
            for(unsigned j=0;j<count;j++){unsigned i=selected[j];struct rd_ep_slot *s=&scan.slots[i];
                if(s->events&0x40000000U){
                    if(s->events&1)(void)rd_ep_change(e,s->fd,EVFILT_READ,EV_DISABLE,0);
                    if(s->events&4)(void)rd_ep_change(e,s->fd,EVFILT_WRITE,EV_DISABLE,0);
                    lck_spin_lock(&rd_ep_registry_lock);if(e->slots[i].generation==s->generation)e->slots[i].armed=false;lck_spin_unlock(&rd_ep_registry_lock);
                }
            }
            *result=(int32_t)count;e->cursor=next;
        }
    }
out:lck_mtx_unlock(&e->operation);fp_drop(p,fd,fp,0);return error;
}
