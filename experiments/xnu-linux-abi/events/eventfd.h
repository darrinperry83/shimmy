/* Linux eventfd counter; XNU fileglob, descriptor references and wait queues. */
#include <sys/select.h>
#include <libkern/section_keywords.h>
#include <sys/eventvar.h>
#include <sys/ioctl.h>
#include <sys/uio_internal.h>
static LCK_GRP_DECLARE(rd_eventfd_locks,"rd_linux_eventfd");
struct rd_eventfd {
    lck_mtx_t lock;
    struct selinfo select;
    uint64_t count;
    bool semaphore;
};
static void rd_eventfd_wake(struct rd_eventfd *e)
{ wakeup(e);selwakeup(&e->select);KNOTE(&e->select.si_note,1); }
static int rd_eventfd_read(struct fileproc *fp,struct uio *uio,int flags,vfs_context_t ctx)
{
    (void)flags;(void)ctx;
    if(uio_resid(uio)<8)return EINVAL;
    struct rd_eventfd *e=fp_get_data(fp);int error=0;uint64_t value;
    lck_mtx_lock(&e->lock);
    while(!e->count){
        if(fileproc_get_vflags(fp)&FPV_DRAIN){error=EBADF;break;}
        if(fp->fp_glob->fg_flag&FNONBLOCK){error=EAGAIN;break;}
        error=msleep(e,&e->lock,PWAIT|PCATCH,"linux_eventfd_read",0);
        if(error)break;
    }
    if(error){lck_mtx_unlock(&e->lock);return error;}
    value=e->semaphore?1:e->count;e->count-=value;rd_eventfd_wake(e);
    lck_mtx_unlock(&e->lock);
    /* Linux consumes the counter even if the destination copy faults. */
    return uiomove((caddr_t)&value,8,uio);
}
static int rd_eventfd_write(struct fileproc *fp,struct uio *uio,int flags,vfs_context_t ctx)
{
    (void)flags;(void)ctx;
    if(uio_resid(uio)<8)return EINVAL;
    uint64_t value;uio_t copy=uio_duplicate(uio);if(!copy)return ENOMEM;
    int error=uiomove((caddr_t)&value,8,copy);uio_free(copy);
    if(error)return error;if(value==UINT64_MAX)return EINVAL;
    struct rd_eventfd *e=fp_get_data(fp);lck_mtx_lock(&e->lock);
    while(value>UINT64_MAX-1-e->count){
        if(fileproc_get_vflags(fp)&FPV_DRAIN){error=EBADF;break;}
        if(fp->fp_glob->fg_flag&FNONBLOCK){error=EAGAIN;break;}
        error=msleep(e,&e->lock,PWAIT|PCATCH,"linux_eventfd_write",0);
        if(error)break;
    }
    if(!error){e->count+=value;uio_update(uio,8);rd_eventfd_wake(e);}
    lck_mtx_unlock(&e->lock);return error;
}
static int rd_eventfd_select(struct fileproc *fp,int which,void *wql,vfs_context_t ctx)
{
    struct rd_eventfd *e=fp_get_data(fp);lck_mtx_lock(&e->lock);
    int ready=which==FREAD?e->count>0:which==FWRITE?e->count<UINT64_MAX-1:0;
    if(!ready)selrecord(vfs_context_proc(ctx),&e->select,wql);
    lck_mtx_unlock(&e->lock);return ready;
}
static int rd_eventfd_ioctl(struct fileproc *fp,u_long cmd,caddr_t data,vfs_context_t ctx)
{ (void)fp;(void)data;(void)ctx;return cmd==FIONBIO?0:ENOTTY; }
static int rd_eventfd_drain(struct fileproc *fp,vfs_context_t ctx)
{
    (void)ctx;struct rd_eventfd *e=fp_get_data(fp);lck_mtx_lock(&e->lock);
    rd_eventfd_wake(e);lck_mtx_unlock(&e->lock);return 0;
}
static int rd_eventfd_close(struct fileglob *fg,vfs_context_t ctx)
{
    (void)ctx;struct rd_eventfd *e=fg_get_data(fg);lck_mtx_lock(&e->lock);
    selthreadclear(&e->select);lck_mtx_unlock(&e->lock);
    lck_mtx_destroy(&e->lock,&rd_eventfd_locks);kfree_type(struct rd_eventfd,e);return 0;
}
/* Native knote lifetime/locking follows the pipe filter contract. */
static int rd_eventfd_ready(struct knote *kn,struct kevent_qos_s *kev)
{
    struct rd_eventfd *e=knote_kn_hook_get_raw(kn);
    int ready=kn->kn_filter==EVFILT_READ?e->count>0:e->count<UINT64_MAX-1;
    if(ready&&kev)knote_fill_kevent(kn,kev,8);
    return ready;
}
static int rd_eventfd_event(struct knote *kn,long hint)
{ (void)hint;return rd_eventfd_ready(kn,NULL); }
static void rd_eventfd_detach(struct knote *kn)
{
    struct rd_eventfd *e=knote_kn_hook_get_raw(kn);lck_mtx_lock(&e->lock);
    KNOTE_DETACH(&e->select.si_note,kn);lck_mtx_unlock(&e->lock);
}
static int rd_eventfd_process(struct knote *kn,struct kevent_qos_s *kev)
{
    struct rd_eventfd *e=knote_kn_hook_get_raw(kn);lck_mtx_lock(&e->lock);
    int ready=rd_eventfd_ready(kn,kev);lck_mtx_unlock(&e->lock);return ready;
}
static int rd_eventfd_touch(struct knote *kn,struct kevent_qos_s *kev)
{ (void)kev;return rd_eventfd_process(kn,NULL); }
SECURITY_READ_ONLY_EARLY(struct filterops) rd_eventfd_filtops={
    .f_isfd=1,.f_detach=rd_eventfd_detach,.f_event=rd_eventfd_event,
    .f_touch=rd_eventfd_touch,.f_process=rd_eventfd_process,
};
static int rd_eventfd_kqfilter(struct fileproc *fp,struct knote *kn,struct kevent_qos_s *kev)
{
    (void)kev;
    if(kn->kn_filter!=EVFILT_READ&&kn->kn_filter!=EVFILT_WRITE){knote_set_error(kn,EINVAL);return 0;}
    struct rd_eventfd *e=fp_get_data(fp);lck_mtx_lock(&e->lock);
    kn->kn_filtid=EVFILTID_RDXNU_EVENTFD;knote_kn_hook_set_raw(kn,e);
    KNOTE_ATTACH(&e->select.si_note,kn);int ready=rd_eventfd_ready(kn,NULL);
    lck_mtx_unlock(&e->lock);return ready;
}
static const struct fileops rd_eventfd_ops={
    .fo_type=DTYPE_RDXNU_EVENTFD,.fo_read=rd_eventfd_read,.fo_write=rd_eventfd_write,
    .fo_ioctl=rd_eventfd_ioctl,.fo_select=rd_eventfd_select,.fo_close=rd_eventfd_close,
    .fo_drain=rd_eventfd_drain,.fo_kqfilter=rd_eventfd_kqfilter,
};
int rd_linux_eventfd(struct proc *p,uint32_t initial,uint32_t flags,int32_t *result)
{
    if(flags&~0x80801U)return EINVAL;
    struct rd_eventfd *e=kalloc_type(struct rd_eventfd,Z_WAITOK|Z_ZERO);
    if(!e)return ENOMEM;
    lck_mtx_init(&e->lock,&rd_eventfd_locks,LCK_ATTR_NULL);e->count=initial;e->semaphore=flags&1;
    struct fileproc *fp;int fd;
    int error=falloc_withinit(p,current_cached_proc_cred(p),vfs_context_current(),&fp,&fd,NULL,NULL);
    if(error){lck_mtx_destroy(&e->lock,&rd_eventfd_locks);kfree_type(struct rd_eventfd,e);return error;}
    fp->fp_glob->fg_flag=FREAD|FWRITE|((flags&0x800)?FNONBLOCK:0);
    fp->fp_glob->fg_ops=&rd_eventfd_ops;fp_set_data(fp,e);
    if(flags&0x80000)fp->fp_flags|=FP_CLOEXEC;
    proc_fdlock(p);procfdtbl_releasefd(p,fd,NULL);fp_drop(p,fd,fp,1);proc_fdunlock(p);
    *result=fd;return 0;
}

bool rd_linux_eventfd_getfl(struct proc *p,int fd,int64_t *result)
{
    struct fileproc *fp;int error=fp_lookup(p,fd,&fp,0);
    if(error)return false;
    bool match=fp->fp_glob->fg_ops==&rd_eventfd_ops;
    if(match)*result=2|((fp->fp_glob->fg_flag&FNONBLOCK)?0x800:0);
    fp_drop(p,fd,fp,0);return match;
}
