/* Startup contracts reuse src/syscalls.c; XNU supplies VM and native threads. */
#include <sys/resourcevar.h>
#include <sys/user.h>
#include <sys/signalvar.h>
#include "rdxnu_linux_state.h"
#include "rdxnu_linux_machine.inc"
_Static_assert(offsetof(struct uthread, rd_linux_enabled) - offsetof(struct uthread, rd_linux_tls) == 24, "entry metadata layout");
const uint64_t rd_linux_uthread_tail_offset = offsetof(struct uthread, rd_linux_tls);

static LCK_GRP_DECLARE(rd_linux_heap_group, "rdxnu_linux_heap");

static struct rd_linux_proc_state *rd_linux_state_new(void)
{
    struct rd_linux_proc_state *s = kalloc_type(struct rd_linux_proc_state, Z_WAITOK | Z_ZERO | Z_NOFAIL);
    lck_mtx_init(&s->heap_lock, &rd_linux_heap_group, LCK_ATTR_NULL);
    return s;
}
void rd_linux_state_free(struct proc *p)
{
    struct rd_linux_proc_state *s = p->p_stats->ps_rd_linux;
    if (!s) return;
    p->p_stats->ps_rd_linux = NULL;
    lck_mtx_destroy(&s->heap_lock, &rd_linux_heap_group);
    kfree_type(struct rd_linux_proc_state,s);
}
void rd_linux_state_fork(struct proc *parent, struct proc *child)
{
    struct rd_linux_proc_state *old = parent->p_stats->ps_rd_linux;
    if (!old) return;
    struct rd_linux_proc_state *s = rd_linux_state_new();
    lck_mtx_lock(&old->heap_lock);
    s->heap_base = old->heap_base; s->heap_current = old->heap_current;
    lck_mtx_unlock(&old->heap_lock);
    child->p_stats->ps_rd_linux = s;
}

static int rd_linux_signal_native(unsigned number)
{
    static const uint8_t numbers[32] = {0,1,2,3,4,5,6,10,8,9,30,11,31,13,14,15,
                                       0,20,19,17,18,21,22,16,24,25,26,27,28,23,0,12};
    return number < 32 ? numbers[number] : 0;
}
static sigset_t rd_linux_mask_native(uint64_t mask)
{
    sigset_t out = 0;
    for (unsigned i=1;i<32;i++) {
        int native = rd_linux_signal_native(i);
        if (native && (mask & (1ULL<<(i-1)))) out |= sigmask(native);
    }
    return out & ~sigcantmask;
}
static uint64_t rd_linux_mask_from_native(sigset_t mask)
{
    uint64_t out = 0;
    for (unsigned i=1;i<32;i++) {
        int native = rd_linux_signal_native(i);
        if (native && (mask & sigmask(native))) out |= 1ULL<<(i-1);
    }
    return out;
}

static void rd_linux_state_exec(struct proc *p, thread_t thread, bool linux, uint64_t base)
{
    /* Ordinary Darwin exec must not touch reserved machine state at all. */
    if (!linux && !p->p_stats->ps_rd_linux) return;
    rd_linux_state_free(p);
    uint64_t mask=0;
    if (linux) {
        struct rd_linux_proc_state *s = rd_linux_state_new();
        s->heap_base = s->heap_current = base;
        p->p_stats->ps_rd_linux = s;
        struct uthread *ut = get_bsdthread_info(thread);
        mask=rd_linux_mask_from_native(ut->uu_sigmask);
    }
    rd_linux_machine_exec(thread,linux,mask);
}

static int64_t rd_linux_brk(struct proc *p, uint64_t want)
{
    struct rd_linux_proc_state *s = p->p_stats->ps_rd_linux;
    if (!s) return 0;
    lck_mtx_lock(&s->heap_lock);
    uint64_t old = s->heap_current;
    if (!want || want < s->heap_base || want > INT64_MAX - RD_ELF_PAGE) goto out;
    if (want - s->heap_base > proc_limitgetcur(p,RLIMIT_DATA)) goto out;
    uint64_t before = (old + RD_ELF_PAGE - 1) & ~(RD_ELF_PAGE-1);
    uint64_t after = (want + RD_ELF_PAGE - 1) & ~(RD_ELF_PAGE-1);
    vm_map_t map = get_task_map(proc_task(p));
    kern_return_t kr = KERN_SUCCESS;
    if (after > before) {
        mach_vm_offset_t address = before;
        kr = mach_vm_allocate_kernel(map,&address,after-before,VM_MAP_KERNEL_FLAGS_FIXED());
    } else if (after < before) {
        kr = mach_vm_deallocate(map,after,before-after);
    }
    if (kr == KERN_SUCCESS) s->heap_current = want;
out:
    old = s->heap_current;
    lck_mtx_unlock(&s->heap_lock);
    return old; /* Linux brk failure returns the old break, not -errno. */
}

struct rd_linux_sigaction { uint64_t handler,flags,restorer,mask; };
static int64_t rd_linux_sigaction(struct proc *p, uint64_t number, user_addr_t action,
                                 user_addr_t previous, uint64_t size)
{
    int sig = rd_linux_signal_native((unsigned)number);
    if (size != 8 || !sig || number>64 || sig==SIGKILL || sig==SIGSTOP) return -22;
    struct rd_linux_sigaction in = {}, out = {};
    if (action && copyin(action,&in,sizeof(in))) return -14;
    /* Caught Linux handlers need Linux signal frames and rt_sigreturn. */
    if (action && in.handler > 1) return -95;
    if (action && (in.flags & ~0xdc000007ULL)) return -22;
    if (previous) {
        struct sigacts *ps = &p->p_sigacts;
        sigset_t bit = sigmask(sig);
        out.handler = SIGACTION(p,sig);
        out.mask = rd_linux_mask_from_native(ps->ps_catchmask[sig]);
        if (!(ps->ps_sigintr & bit)) out.flags |= 0x10000000;
        if (ps->ps_sigonstack & bit) out.flags |= 0x08000000;
        if (ps->ps_siginfo & bit) out.flags |= 4;
        if (ps->ps_signodefer & bit) out.flags |= 0x40000000;
        if (ps->ps_sigreset & bit) out.flags |= 0x80000000;
        if (sig==SIGCHLD && (p->p_flag & P_NOCLDSTOP)) out.flags |= 1;
        if (sig==SIGCHLD && (p->p_flag & P_NOCLDWAIT)) out.flags |= 2;
        if (copyout(&out,previous,sizeof(out))) return -14;
    }
    if (!action) return 0;
    struct __kern_sigaction native = {};
    native.sa_handler = in.handler;
    native.sa_mask = rd_linux_mask_native(in.mask);
    if (in.flags & 1) native.sa_flags |= SA_NOCLDSTOP;
    if (in.flags & 2) native.sa_flags |= SA_NOCLDWAIT;
    if (in.flags & 4) native.sa_flags |= SA_SIGINFO;
    if (in.flags & 0x08000000) native.sa_flags |= SA_ONSTACK;
    if (in.flags & 0x10000000) native.sa_flags |= SA_RESTART;
    if (in.flags & 0x40000000) native.sa_flags |= SA_NODEFER;
    if (in.flags & 0x80000000) native.sa_flags |= SA_RESETHAND;
    int error = setsigvec(p,current_thread(),sig,&native,FALSE);
    return error ? -rd_linux_errno(error) : 0;
}

/* Reuse the shim's clock-number/error contract; XNU owns the boot epoch.
 * MONOTONIC uses native awake uptime; BOOTTIME includes native sleep time.
 * Linux-specific NTP discipline and CPU clock IDs are not qualified here.
 */
#include <kern/clock.h>
#include <mach/mach_time.h>
static int64_t rd_linux_clock(uint64_t nr, int32_t clock_id, user_addr_t address)
{
    if (clock_id!=0 && clock_id!=1 && clock_id!=4 && clock_id!=7) return -22;
    struct { int64_t sec, nsec; } result={0,0};
    if (nr==114) {
        if (!address) return 0; /* clock_getres permits a null output. */
        mach_timebase_info_data_t base;
        clock_timebase_info(&base);
        uint64_t ns=((uint64_t)base.numer+base.denom-1)/base.denom;
        if (!ns) ns=1;
        result.sec=ns/1000000000ULL;result.nsec=ns%1000000000ULL;
    } else if (clock_id==0) {
        clock_sec_t secs;clock_nsec_t nsecs;
        clock_get_calendar_nanotime(&secs,&nsecs);
        result.sec=secs;result.nsec=nsecs;
    } else {
        uint64_t ns;
        absolutetime_to_nanoseconds(clock_id==7 ? mach_continuous_time() : mach_absolute_time(),&ns);
        result.sec=ns/1000000000ULL;result.nsec=ns%1000000000ULL;
    }
    return copyout(&result,address,sizeof(result)) ? -14 : 0;
}

static int64_t rd_linux_startup(struct proc *p, arm_saved_state_t *state, uint64_t nr)
{
    uint64_t a0=get_saved_state_reg(state,0),a1=get_saved_state_reg(state,1);
    uint64_t a2=get_saved_state_reg(state,2),a3=get_saved_state_reg(state,3);
    if (nr==113 || nr==114) return rd_linux_clock(nr,(int32_t)a0,a1);
    if (nr==214) return rd_linux_brk(p,a0);
    if (nr==178) return proc_getpid(p); /* Single-thread ELF checkpoint only. */
    if (nr==96) {
        rd_linux_machine_set_clear_tid(current_thread(),a0);
        return proc_getpid(p);
    }
    if (nr==48) {
        struct faccessat_args args = {};
        args.fd=rd_linux_dirfd(a0); args.path=a1; args.amode=(int)a2; args.flag=0;
        int32_t unused=0;
        int error=faccessat(p,&args,&unused);
        return error ? -rd_linux_errno(error) : 0;
    }
    if (nr==278) {
        if ((a2 & ~7ULL) || ((a2 & 6)==6) || a1>INT64_MAX) return -22;
        unsigned char bytes[256]; uint64_t done=0;
        /* Linux permits short reads. Bound stack usage and work per entry. */
        size_t count=(size_t)MIN(a1,sizeof(bytes));
        if (count) {
            read_random(bytes,(unsigned)count);
            if (copyout(bytes,a0,count)) return -14;
            done=count;
        }
        return done;
    }
    if (nr==261) {
        if (a0 && a0!=(uint64_t)proc_getpid(p)) return -95;
        if (a2) return -95; /* Resource-limit mutation is a later contract. */
        static const int resources[]={RLIMIT_CPU,RLIMIT_FSIZE,RLIMIT_DATA,RLIMIT_STACK,
            RLIMIT_CORE,RLIMIT_RSS,RLIMIT_NPROC,RLIMIT_NOFILE,RLIMIT_MEMLOCK,RLIMIT_AS};
        if (a1>=sizeof(resources)/sizeof(resources[0])) return -22;
        struct rlimit native=proc_limitget(p,resources[a1]);
        uint64_t limits[2]={native.rlim_cur==RLIM_INFINITY ? UINT64_MAX : native.rlim_cur,
                           native.rlim_max==RLIM_INFINITY ? UINT64_MAX : native.rlim_max};
        return a3 && copyout(limits,a3,sizeof(limits)) ? -14 : 0;
    }
    if (nr==167 && (a0==15 || a0==16)) {
        char name[16]={0};
        if (a0==15) {
            for (unsigned i=0;i<15;i++) {
                if (copyin(a1+i,&name[i],1)) return -14;
                if (!name[i]) break;
            }
            thread_set_thread_name(current_thread(),name);
        } else {
            char native[MAXTHREADNAMESIZE]={0};
            thread_get_thread_name(current_thread(),native);
            strlcpy(name,native,sizeof(name));
            if (copyout(name,a1,sizeof(name))) return -14;
        }
        return 0;
    }
    if (nr==134) return rd_linux_sigaction(p,a0,a1,a2,a3);
    if (nr==135) {
        if (a3!=8 || (a1 && a0>2)) return -22;
        thread_t thread=current_thread();
        uint64_t before=rd_linux_machine_mask(thread), mask=0;
        if (a1 && copyin(a1,&mask,8)) return -14;
        if (a2 && copyout(&before,a2,8)) return -14;
        if (a1) {
            mask &= ~((1ULL<<8)|(1ULL<<18)); /* KILL and STOP cannot be blocked. */
            uint64_t next=a0==0 ? before|mask : a0==1 ? before&~mask : mask;
            rd_linux_machine_set_mask(thread,next);
            current_uthread()->uu_sigmask=rd_linux_mask_native(next);
            signal_setast(thread);
        }
        return 0;
    }
    return -38;
}
