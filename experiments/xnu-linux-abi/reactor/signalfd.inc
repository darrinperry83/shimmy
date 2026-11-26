/* Included after kern_sig.c's native helpers. Pending bits remain native. */
#include <sys/eventvar.h>
#include <sys/uio_internal.h>
#include <sys/ioctl.h>
#include "rdxnu_linux_state.h"
#define RD_NOTE_SIGNAL_READY 0x01000000L
static LCK_GRP_DECLARE(rd_sigfd_group,"rd_linux_signalfd");
static LCK_SPIN_DECLARE(rd_sigmeta_lock,&rd_sigfd_group);
static int rd_sig_native(unsigned signal)
{static const uint8_t map[32]={0,1,2,3,4,5,6,10,8,9,30,11,31,13,14,15,0,20,19,17,18,21,22,16,24,25,26,27,28,23,0,12};return signal<32?map[signal]:0;}
bool rd_linux_keep_blocked_default(struct proc *p,int signal)
{
    if(!(os_atomic_load(&p->p_ladvflag,relaxed)&P_RDXNU_LINUX)||SIGACTION(p,signal)!=SIG_DFL)return false;
    struct uthread *uth;TAILQ_FOREACH(uth,&p->p_uthlist,uu_list)if(uth->uu_sigmask&sigmask(signal))return true;
    return false;
}
static int rd_sig_linux(int signal)
{for(unsigned i=1;i<32;i++)if(rd_sig_native(i)==signal)return (int)i;return 0;}
struct rd_linux_proc_state *rd_signal_state_detach(struct proc *p)
{
    lck_spin_lock(&rd_sigmeta_lock);struct rd_linux_proc_state *s=p->p_stats->ps_rd_linux;p->p_stats->ps_rd_linux=NULL;
    lck_spin_unlock(&rd_sigmeta_lock);return s;
}
void rd_signal_state_assign(struct proc *p,struct rd_linux_proc_state *s)
{lck_spin_lock(&rd_sigmeta_lock);p->p_stats->ps_rd_linux=s;lck_spin_unlock(&rd_sigmeta_lock);}
void rd_linux_signal_post(struct proc *p,int signal,bool already)
{
    if(!(os_atomic_load(&p->p_ladvflag,relaxed)&P_RDXNU_LINUX))return;
    int linux_signal=rd_sig_linux(signal);
    if(linux_signal&&!already){lck_spin_lock(&rd_sigmeta_lock);
        struct rd_linux_proc_state *s=p->p_stats?p->p_stats->ps_rd_linux:NULL;
        if(s){struct rd_signal_meta *m=&s->signals[linux_signal];
            if(signal==SIGCHLD){m->pid=p->si_pid;m->uid=p->si_uid;m->code=p->si_code;
                m->status=WIFSIGNALED(p->si_status)?rd_sig_linux(WTERMSIG(p->si_status)):WEXITSTATUS(p->si_status)&255;
                if(WIFSIGNALED(p->si_status))m->code=WCOREDUMP(p->si_status)?CLD_DUMPED:CLD_KILLED;
            }else{m->pid=proc_getpid(current_proc());m->uid=kauth_getruid();m->code=0;m->status=0;}}
        lck_spin_unlock(&rd_sigmeta_lock);}
    /* This hint is separate from NOTE_SIGNAL, preserving Darwin signal counts. */
    proc_knote(p,RD_NOTE_SIGNAL_READY|signal);wakeup(&p->p_siglist);
}
struct rd_sigfd {uint32_t mask;};
struct rd_sig_record {
    uint32_t signo;int32_t error,code;uint32_t pid,uid;int32_t fd;
    uint32_t tid,band,overrun,trapno;int32_t status,value;
    uint64_t pointer,utime,stime,addr;uint16_t addr_lsb,pad;int32_t syscall;
    uint64_t call_addr;uint32_t arch;unsigned char spare[28];
};
_Static_assert(sizeof(struct rd_sig_record)==128,"Linux signalfd record");
static sigset_t rd_sigfd_pending_locked(struct proc *p,sigset_t mask)
{
    sigset_t pending=p->p_siglist&mask;struct uthread *uth;
    TAILQ_FOREACH(uth,&p->p_uthlist,uu_list)pending|=uth->uu_siglist&mask;
    return pending;
}
static int rd_sigfd_take(struct proc *p,struct fileproc *fp,struct rd_sig_record *record,bool block)
{
    struct rd_sigfd *s=fp_get_data(fp);int error=0;proc_lock(p);
    for(;;){sigset_t mask=os_atomic_load(&s->mask,relaxed),pending=rd_sigfd_pending_locked(p,mask);
        if(pending){int native=0,linux_signal=0;for(int i=1;i<32;i++)if(rd_sig_native(i)&&(pending&sigmask(rd_sig_native(i)))){linux_signal=i;native=rd_sig_native(i);break;}
            sigset_t bit=sigmask(native);
            struct uthread *uth;bool taken=false;
            TAILQ_FOREACH(uth,&p->p_uthlist,uu_list)if(uth->uu_siglist&bit){uth->uu_siglist&=~bit;taken=true;break;}
            if(!taken)p->p_siglist&=~bit;
            bzero(record,sizeof(*record));record->signo=linux_signal;
            lck_spin_lock(&rd_sigmeta_lock);struct rd_linux_proc_state *state=p->p_stats->ps_rd_linux;
            if(state&&linux_signal){struct rd_signal_meta *m=&state->signals[linux_signal];record->pid=m->pid;record->uid=m->uid;record->code=m->code;record->status=m->status;*m=(struct rd_signal_meta){0};}
            lck_spin_unlock(&rd_sigmeta_lock);break;
        }
        if(fileproc_get_vflags(fp)&FPV_DRAIN){error=EBADF;break;}
        if(!block||(fp->fp_glob->fg_flag&FNONBLOCK)){error=EAGAIN;break;}
        error=msleep(&p->p_siglist,&p->p_mlock,PWAIT|PCATCH,"linux_signalfd",0);if(error)break;
    }
    proc_unlock(p);return error;
}
static int rd_sigfd_read(struct fileproc *fp,struct uio *uio,int flags,vfs_context_t ctx)
{
    (void)flags;struct proc *p=vfs_context_proc(ctx);
    if(!(os_atomic_load(&p->p_ladvflag,relaxed)&P_RDXNU_LINUX))return ENOTSUP;
    if(uio_resid(uio)<128)return EINVAL;bool first=true;
    while(uio_resid(uio)>=128){struct rd_sig_record record;int error=rd_sigfd_take(p,fp,&record,first);
        if(error)return !first&&error==EAGAIN?0:error;
        error=uiomove((caddr_t)&record,sizeof(record),uio);if(error)return error;first=false;}
    return 0;
}
static int rd_sigfd_write(struct fileproc *fp,struct uio *uio,int flags,vfs_context_t ctx)
{(void)fp;(void)uio;(void)flags;(void)ctx;return EINVAL;}
static int rd_sigfd_ioctl(struct fileproc *fp,u_long cmd,caddr_t data,vfs_context_t ctx)
{(void)fp;(void)data;(void)ctx;return cmd==FIONBIO?0:ENOTTY;}
static int rd_sigfd_select(struct fileproc *fp,int which,void *wql,vfs_context_t ctx)
{(void)fp;(void)which;(void)wql;(void)ctx;return 0;}
static int rd_sigfd_close(struct fileglob *fg,vfs_context_t ctx)
{(void)ctx;struct rd_sigfd *s=fg_get_data(fg);kfree_type(struct rd_sigfd,s);return 0;}
static int rd_sigfd_drain(struct fileproc *fp,vfs_context_t ctx)
{(void)fp;struct proc *p=vfs_context_proc(ctx);proc_lock(p);wakeup(&p->p_siglist);proc_unlock(p);return 0;}
static int rd_sigfd_event(struct knote *kn,long hint)
{
    struct rd_sigfd *s=fp_get_data(kn->kn_fp);
    if(hint&RD_NOTE_SIGNAL_READY){int signal=(int)(hint&0xff);return signal>0&&signal<NSIG&&(os_atomic_load(&s->mask,relaxed)&sigmask(signal));}
    return 0;
}
static int rd_sigfd_process(struct knote *kn,struct kevent_qos_s *kev)
{
    proc_t p=knote_kn_hook_get_raw(kn);if(!p)return 0;struct rd_sigfd *s=fp_get_data(kn->kn_fp);
    proc_lock(p);bool ready=rd_sigfd_pending_locked(p,os_atomic_load(&s->mask,relaxed))!=0;proc_unlock(p);
    if(ready&&kev)knote_fill_kevent(kn,kev,128);return ready;
}
static int rd_sigfd_touch(struct knote *kn,struct kevent_qos_s *kev)
{(void)kev;return rd_sigfd_process(kn,NULL);}
static void rd_sigfd_detach(struct knote *kn)
{
    proc_klist_lock();proc_t p=knote_kn_hook_get_raw(kn);
    if(p){KNOTE_DETACH(&p->p_klist,kn);knote_kn_hook_set_raw(kn,NULL);}
    proc_klist_unlock();
}
SECURITY_READ_ONLY_EARLY(struct filterops) rd_sigfd_filtops={.f_isfd=1,.f_detach=rd_sigfd_detach,
    .f_event=rd_sigfd_event,.f_touch=rd_sigfd_touch,.f_process=rd_sigfd_process};
static int rd_sigfd_kqfilter(struct fileproc *fp,struct knote *kn,struct kevent_qos_s *kev)
{
    (void)fp;(void)kev;if(kn->kn_filter!=EVFILT_READ){knote_set_error(kn,ENOTSUP);return 0;}
    struct proc *p=current_proc();if(!(os_atomic_load(&p->p_ladvflag,relaxed)&P_RDXNU_LINUX)){knote_set_error(kn,ENOTSUP);return 0;}
    kn->kn_filtid=EVFILTID_RDXNU_SIGNALFD;knote_kn_hook_set_raw(kn,p);
    proc_lock(p);proc_klist_lock();KNOTE_ATTACH(&p->p_klist,kn);proc_klist_unlock();
    struct rd_sigfd *s=fp_get_data(fp);int ready=rd_sigfd_pending_locked(p,os_atomic_load(&s->mask,relaxed))!=0;proc_unlock(p);return ready;
}
static const struct fileops rd_sigfd_ops={.fo_type=DTYPE_RDXNU_SIGNALFD,.fo_read=rd_sigfd_read,.fo_write=rd_sigfd_write,
    .fo_ioctl=rd_sigfd_ioctl,.fo_select=rd_sigfd_select,.fo_close=rd_sigfd_close,.fo_drain=rd_sigfd_drain,.fo_kqfilter=rd_sigfd_kqfilter};
int rd_linux_signalfd(struct proc *p,int fd,user_addr_t address,uint64_t size,uint32_t flags,int32_t *result)
{
    if(size!=8||(flags&~0x80800U))return EINVAL;uint64_t linux_mask;if(copyin(address,&linux_mask,8))return EFAULT;
    linux_mask&=~((1ULL<<8)|(1ULL<<18));sigset_t mask=0;
    for(unsigned i=1;i<=64;i++)if(linux_mask&(1ULL<<(i-1))){int native=rd_sig_native(i);if(!native)return ENOTSUP;mask|=sigmask(native);}
    if(mask&~current_uthread()->uu_sigmask)return ENOTSUP;
    if(fd!=-1){struct fileproc *fp;int error=fp_lookup(p,fd,&fp,0);if(error)return error;
        if(fp->fp_glob->fg_ops!=&rd_sigfd_ops)error=EINVAL;
        else{struct rd_sigfd *s=fp_get_data(fp);os_atomic_store(&s->mask,mask,relaxed);*result=fd;
            proc_lock(p);for(int sig=1;sig<NSIG;sig++)if(mask&sigmask(sig))proc_knote(p,RD_NOTE_SIGNAL_READY|sig);proc_unlock(p);}
        fp_drop(p,fd,fp,0);return error;}
    struct rd_sigfd *s=kalloc_type(struct rd_sigfd,Z_WAITOK|Z_ZERO);if(!s)return ENOMEM;s->mask=mask;
    struct fileproc *fp;int newfd;int error=falloc_withinit(p,current_cached_proc_cred(p),vfs_context_current(),&fp,&newfd,NULL,NULL);
    if(error){kfree_type(struct rd_sigfd,s);return error;}
    fp->fp_glob->fg_flag=FREAD|FWRITE|((flags&0x800)?FNONBLOCK:0);fp->fp_glob->fg_ops=&rd_sigfd_ops;fp_set_data(fp,s);
    if(flags&0x80000)fp->fp_flags|=FP_CLOEXEC;
    proc_fdlock(p);procfdtbl_releasefd(p,newfd,NULL);fp_drop(p,newfd,fp,1);proc_fdunlock(p);*result=newfd;return 0;
}
