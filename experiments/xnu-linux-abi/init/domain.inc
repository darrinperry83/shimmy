/* Membership must outlive pstats, which XNU frees before publishing a zombie.
 * This index contains only stable native unique IDs and owned domain references;
 * native proc tables, child lists, wait status and reap ownership are authoritative.
 * Shadow exec procs share a logical ID and do not add/remove a binding. */
#include <sys/sysctl.h>
#include <pexpert/pexpert.h>
static uint64_t rd_linux_domains_live;
struct rd_linux_domain_binding {
    LIST_ENTRY(rd_linux_domain_binding) link;
    uint64_t identity;
    struct rd_linux_domain *domain;
};
static LIST_HEAD(rd_linux_domain_bucket,rd_linux_domain_binding) rd_linux_domain_bindings[128];
static LCK_GRP_DECLARE(rd_linux_domain_group,"rd_linux_domain");
static LCK_SPIN_DECLARE(rd_linux_domain_lock,&rd_linux_domain_group);

static int rd_linux_domain_bind(struct proc *p,struct rd_linux_domain *domain)
{
    struct rd_linux_domain_binding *fresh=kalloc_type(struct rd_linux_domain_binding,Z_WAITOK|Z_ZERO|Z_NOFAIL);
    fresh->identity=proc_uniqueid(p);fresh->domain=domain;
    struct rd_linux_domain_bucket *bucket=&rd_linux_domain_bindings[fresh->identity&127];
    lck_spin_lock(&rd_linux_domain_lock);
    struct rd_linux_domain_binding *entry;
    LIST_FOREACH(entry,bucket,link)if(entry->identity==fresh->identity)break;
    if(!entry)LIST_INSERT_HEAD(bucket,fresh,link);
    lck_spin_unlock(&rd_linux_domain_lock);
    if(entry){kfree_type(struct rd_linux_domain_binding,fresh);return EBUSY;}
    return 0; /* Binding takes the caller's owned domain reference. */
}

struct rd_linux_domain *rd_linux_domain_hold(struct proc *p)
{
    uint64_t identity=proc_uniqueid(p);
    lck_spin_lock(&rd_linux_domain_lock);
    struct rd_linux_domain_binding *entry;
    LIST_FOREACH(entry,&rd_linux_domain_bindings[identity&127],link)if(entry->identity==identity)break;
    struct rd_linux_domain *d=entry?entry->domain:NULL;
    if(d)os_ref_retain(&d->references);
    lck_spin_unlock(&rd_linux_domain_lock);
    return d;
}
int rd_linux_domain_inherit(struct proc *parent,struct proc *child)
{
    struct rd_linux_domain *domain=rd_linux_domain_hold(parent);
    if(!domain)return 0;
    int error=rd_linux_domain_bind(child,domain);
    if(error)rd_linux_domain_release(domain);
    return error;
}
void rd_linux_domain_forget(struct proc *p)
{
    if(!p->p_proc_ro||proc_is_shadow(p))return;
    uint64_t identity=proc_uniqueid(p);
    lck_spin_lock(&rd_linux_domain_lock);
    struct rd_linux_domain_binding *entry;
    LIST_FOREACH(entry,&rd_linux_domain_bindings[identity&127],link)if(entry->identity==identity)break;
    if(entry)LIST_REMOVE(entry,link);
    lck_spin_unlock(&rd_linux_domain_lock);
    if(entry){rd_linux_domain_release(entry->domain);kfree_type(struct rd_linux_domain_binding,entry);}
}
void rd_linux_domain_release(struct rd_linux_domain *d)
{
    if(d&&os_ref_release(&d->references)==0){
        os_atomic_dec(&rd_linux_domains_live,relaxed);
        kfree_type(struct rd_linux_domain,d);
    }
}
uint64_t rd_linux_domain_id(struct proc *p)
{
    struct rd_linux_domain *d=rd_linux_domain_hold(p);
    uint64_t identity=d?d->identity:0;rd_linux_domain_release(d);return identity;
}
bool rd_linux_domain_same(struct proc *a,struct proc *b)
{
    struct rd_linux_domain *first=rd_linux_domain_hold(a),*second=rd_linux_domain_hold(b);
    bool same=first==second;
    rd_linux_domain_release(first);rd_linux_domain_release(second);return same;
}
bool rd_linux_domain_target(struct proc *caller,struct proc *target)
{
    /* Retain the qualified, non-domain regression behavior. Domain callers
     * must resolve every target to a held native proc and match membership. */
    return !rd_linux_domain_id(caller)||rd_linux_domain_same(caller,target);
}
bool rd_linux_domain_leader(struct proc *p)
{
    struct rd_linux_domain *d=rd_linux_domain_hold(p);
    bool leader=d&&d->identity==proc_uniqueid(p)&&d->leader_pid==proc_getpid(p);
    rd_linux_domain_release(d);return leader;
}
bool rd_linux_domain_closing(struct proc *p)
{
    struct rd_linux_domain *d=rd_linux_domain_hold(p);
    bool closing=d&&os_atomic_load(&d->closing,acquire);
    rd_linux_domain_release(d);return closing;
}
static int rd_linux_domain_kill_member(proc_t p,void *context)
{
    struct rd_linux_domain *d=context,*member=rd_linux_domain_hold(p);
    if(member==d&&proc_uniqueid(p)!=d->identity&&!proc_is_shadow(p))psignal(p,SIGKILL);
    rd_linux_domain_release(member);return PROC_RETURNED;
}
static int rd_linux_domain_filter(proc_t p,void *context)
{
    struct rd_linux_domain *d=context;
    if(proc_uniqueid(p)==d->identity||proc_is_shadow(p))return 0;
    struct rd_linux_domain *member=rd_linux_domain_hold(p);
    bool matches=member==d;rd_linux_domain_release(member);return matches;
}
void rd_linux_domain_exit(struct proc *p)
{
    if(proc_is_shadow(p)||!rd_linux_domain_leader(p))return;
    struct rd_linux_domain *d=rd_linux_domain_hold(p);
    if(!d)return;
    os_atomic_store(&d->closing,1,release);
    /* Native iteration grants temporary proc references. Do not hold the proc
     * list or metadata lock while signalling, waiting for exec, or freeing. */
    proc_iterate(PROC_ALLPROCLIST,rd_linux_domain_kill_member,d,rd_linux_domain_filter,d);
    rd_linux_domain_release(d);
}

static int rd_linux_domain_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1,arg2,oidp)
    struct proc *p=req->p;
    if(!p||p!=current_proc())return EINVAL;
    if(req->newptr){
        uint32_t command=0;
        if(req->newlen!=sizeof(command))return EINVAL;
        int error=SYSCTL_IN(req,&command,sizeof(command));if(error)return error;
        if(command!=1)return EINVAL;
        if(kauth_cred_getuid(kauth_cred_get())!=0)return EPERM;
        if(!PE_parse_boot_argn("rdxnu_linux_elf",&error,sizeof(error))||!error)return ENOTSUP;
        proc_lock(p);
        struct uthread *first=TAILQ_FIRST(&p->p_uthlist);
        bool single=first&&TAILQ_NEXT(first,uu_list)==NULL;
        proc_unlock(p);
        if(!single)return ENOTSUP;
        if(rd_linux_domain_id(p))return EBUSY;
        struct rd_linux_domain *d=kalloc_type(struct rd_linux_domain,Z_WAITOK|Z_ZERO|Z_NOFAIL);
        os_ref_init(&d->references,NULL);d->identity=proc_uniqueid(p);d->leader_pid=proc_getpid(p);
        os_atomic_inc(&rd_linux_domains_live,relaxed);
        error=rd_linux_domain_bind(p,d);
        if(error){rd_linux_domain_release(d);return error;}
    }
    uint64_t identity=rd_linux_domain_id(p);
    return SYSCTL_OUT(req,&identity,sizeof(identity));
}
SYSCTL_PROC(_kern,OID_AUTO,rdxnu_linux_domain,CTLTYPE_QUAD|CTLFLAG_RW|CTLFLAG_LOCKED,
    0,0,rd_linux_domain_sysctl,"Q","Diagnostic current-process Linux domain admission; native IDs");
static int rd_linux_domains_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1,arg2,oidp)
    uint64_t live=os_atomic_load(&rd_linux_domains_live,relaxed);
    return SYSCTL_OUT(req,&live,sizeof(live));
}
SYSCTL_PROC(_kern,OID_AUTO,rdxnu_linux_domains_live,CTLTYPE_QUAD|CTLFLAG_RD|CTLFLAG_LOCKED,
    0,0,rd_linux_domains_sysctl,"Q","Live owned Linux domain objects");
