// shim.c — top-level integration for the Linux-on-macOS shim (no VM, no Rosetta).
//
// Runs unmodified aarch64 Linux ELF binaries directly on Apple Silicon by loading them into
// this process's address space and intercepting the few instructions that mean "talk to the
// kernel" or "touch thread-local storage".
//
// Interception (planted by gmem_rewrite_exec_insns on every executable segment):
//   svc #0            -> brk #0          : Linux syscall  -> linux/net/gpu dispatch
//   msr tpidr_el0, xN -> brk #(0x100+rt) : guest WRITES its TLS pointer -> captured here
//   mrs xN, tpidr_el0 -> brk #(0x200+rt) : guest READS its TLS pointer  -> answered here
//
// WHY TLS IS VIRTUALIZED (this is load-bearing): macOS OWNS TPIDR_EL0 and restores its own
// per-thread value on every context switch. Keeping the guest's thread pointer in the hardware
// register therefore works only until the first preemption, after which any inline
// `mrs x, tpidr_el0` reads a macOS value and libc dereferences garbage. Virtualizing BOTH
// directions makes the hardware register irrelevant, which is what made this stable.
//
// Threading: Cocoa/Metal own the MAIN thread, so the guest runs on a WORKER thread and traps
// are serviced on the Mach exception-server thread.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <dispatch/dispatch.h>
#include <spawn.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fcntl.h>
#include <mach/mach_vm.h>
#include <mach/mach.h>
#include <mach/arm/thread_state.h>
#include <mach/thread_act.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <pthread/pthread.h>
#include "elfload.h"
#include "syscalls.h"
#include "syscalls_ext.h"
#include "net.h"
#include "bootstrap.h"
#include "gmem.h"
#include "vdso.h"
#include "fastcall.h"
#include "guestthread.h"
#include "execve.h"
#ifndef NO_GPU
#include "x11srv.h"
#include "wlsrv.h"
#include "gpu.h"
#else
#ifdef WL_HEADLESS
#include "wlsrv.h"                                // compositor without Metal (see wlhead_stub.c)
#endif
struct gpu_req;                                   // CLI build: no Metal/Cocoa
static void gpu_init(void){}
static long gpu_hypercall(struct gpu_req* r){ (void)r; return -38; }
static void gpu_run_event_loop(void){ for(;;) pause(); }  // guest thread _exit()s the process
#endif

#define GPU_SYSCALL 0x2000
// B5 research spike: guest -> host Mach-O call channel (src/hostcall.c). Opt-in via
// RDHOSTCALL=1; every op returns -EPERM otherwise, so no existing path can reach it.
#define HOSTCALL_SYSCALL 0x2001
struct hostcall_req;
extern long hostcall(struct hostcall_req* r);
#define BRK_TLS_WRITE 0x100     // brk #(0x100+rt) == msr tpidr_el0, x<rt>
#define BRK_TLS_READ  0x200     // brk #(0x200+rt) == mrs x<rt>, tpidr_el0
#define BRK_CTR_READ  0x400     // brk #(0x400+rt) == mrs x<rt>, ctr_el0  (Darwin SIGILLs on it)
#define BRK_DC_ZVA    0x800     // brk #(0x800+rt) == dc zva, x<rt>  (zero 64 bytes at x<rt>)
#define BRK_MRS_DCZID 0x900     // brk #(0x900+rt) == mrs x<rt>, DCZID_EL0 (return 0x4 = 64-byte line)

extern boolean_t mach_exc_server(mach_msg_header_t*, mach_msg_header_t*);
static int g_verbose;
static int g_sctrace_fx;          // RDFUTEX_SC=1: mirror the syscall trace into the per-pid futex log
void fx_note(const char* fmt, ...);
static unsigned long g_main_base, g_interp_base;   // for fault diagnostics

// The guest's thread pointer is per-thread and lives in the gthread table below, keyed by mach
// thread port. (It used to be one global, which was only correct while the guest was
// single-threaded.) This is the value used before any thread record exists — i.e. the very first
// trap, before the main guest thread has been registered.
//
// *** DO NOT USE THIS AS A THREAD'S TLS HOME. IT IS A BOOTSTRAP VALUE, NOT A HOME. ***
//
// There WAS a second, near-identically named global, `gmem_guest_tpidr` in gmem.c, and every guest
// thread ended up sharing it: gmem_tls_slot() returned it when TSD storage was off, and the binding
// code recorded that as this thread's home unconditionally. Symptoms were glibc's rwlock reading two
// threads as one ("Failed to get RW lock: Resource deadlock avoided") and two threads on one malloc
// arena ("unaligned tcache chunk detected"). It shipped past glibc-gate, symlink-gate, net-gate,
// x18thr and x18mix, because all of those exercise the default arm and the defect lived in the
// fallback. It was deleted, not guarded.
//
// THE TRAP THIS COMMENT EXISTS FOR: if you delete or move that symbol again and the build breaks
// with `use of undeclared identifier 'gmem_guest_tpidr'`, clang will helpfully suggest
// `did you mean 'g_guest_tpidr'?` — this variable, one character different. TAKING THAT SUGGESTION
// COMPILES CLEANLY AND SILENTLY REINTRODUCES THE IDENTICAL BUG UNDER A NEW NAME. The fastest fix
// for the build error is the defect. Run tools/p2tls-gate.sh (all six arms) before believing a
// green build here. (Found by P2, who hit exactly this.)
static unsigned long g_guest_tpidr;

// The fastcall thunk's dispatch. clone(220) is excluded statically when the sites are patched
// (see fc_stub_patch), because do_clone() needs the register file the thunk cannot supply. This
// is the belt to that braces: if a clone ever reaches here — a dynamic syscall number, say — fail
// LOUDLY with -ENOSYS instead of silently doing the wrong thing and hanging.
static long route_syscall(unsigned long nr, unsigned long a0, unsigned long a1, unsigned long a2,
                          unsigned long a3, unsigned long a4, unsigned long a5);
static long route_syscall_fc(unsigned long nr, unsigned long a0, unsigned long a1,
                             unsigned long a2, unsigned long a3, unsigned long a4,
                             unsigned long a5) {
  if (nr == 220 || nr == 93) {
    static int said;
    if (!said) { said = 1;
      fprintf(stderr, "[fastcall] syscall %lu reached the fast path but must not: clone(220) needs "
                      "the register file, exit(93) unwinds off the thunk's native stack. Refusing "
                      "(-ENOSYS); the site should have been excluded statically.\n", nr); }
    return -38;                                    // -ENOSYS
  }
  // RDFCTRACE=1: the fast path bypasses the trap handler, so RDSCTRACE cannot see it. Without
  // this, a hang on the fast path shows the LAST TRAPPED syscall and nothing about the one that
  // actually stuck.
  static int trace = -1;
  if (trace < 0) trace = getenv("RDFCTRACE") != 0;
  if (trace) { fprintf(stderr, "[fc pid=%d] nr=%lu(%#lx,%#lx,%#lx)\n", getpid(), nr, a0, a1, a2); fflush(stderr); }
  long r = route_syscall(nr, a0, a1, a2, a3, a4, a5);
  if (trace) { fprintf(stderr, "[fc pid=%d] nr=%lu -> %ld\n", getpid(), nr, r); fflush(stderr); }
  return r;
}

// Shared routing for both the trap handler and the fastcall thunk.
// RDSCENTER=1 logs syscalls on ENTRY. The normal trace prints on return, so a thread blocked
// INSIDE a call leaves no line at all — which makes a hang look like "it just stopped doing
// syscalls" and hides the one call that matters.
static int g_sc_enter = -1;

// ---- P2 MEASUREMENT-ONLY INSTRUMENT (RDCENSUS=1) --------------------------------------------
// Per-syscall-number count + cumulative nanoseconds spent INSIDE the dispatch body, dumped at
// exit. The point is to separate "trap round trip" (count x per-trap cost) from "work the
// handler actually does" (this timer). Not for merge; a measurement scaffold only.
#include <mach/mach_time.h>
static int g_census = -1;
#define CENSUS_MAX 600
static volatile unsigned long long g_cn[CENSUS_MAX], g_ct[CENSUS_MAX];
static volatile unsigned long long g_cn_other, g_ct_other;
static void trapcnt_dump(void);   // P2: defined after the counters (line ~850)
static void census_dump(void) {
  if (g_census != 1) return;
  trapcnt_dump();
  mach_timebase_info_data_t tb; mach_timebase_info(&tb);
  unsigned long long tot = 0, tott = 0;
  for (int i = 0; i < CENSUS_MAX; i++) { tot += g_cn[i]; tott += g_ct[i]; }
  tot += g_cn_other; tott += g_ct_other;
  fprintf(stderr, "[census] TOTAL syscalls=%llu body_ns=%llu\n",
          tot, (unsigned long long)(tott * tb.numer / tb.denom));
  for (int i = 0; i < CENSUS_MAX; i++)
    if (g_cn[i]) fprintf(stderr, "[census] nr=%-4d n=%-8llu body_ns=%llu\n", i,
                         g_cn[i], (unsigned long long)(g_ct[i] * tb.numer / tb.denom));
  if (g_cn_other) fprintf(stderr, "[census] nr=OTHER n=%llu body_ns=%llu\n", g_cn_other,
                          (unsigned long long)(g_ct_other * tb.numer / tb.denom));
  fflush(stderr);
}

static long route_syscall(unsigned long nr, unsigned long a0, unsigned long a1, unsigned long a2,
                          unsigned long a3, unsigned long a4, unsigned long a5);
static long route_syscall_census(unsigned long nr, unsigned long a0, unsigned long a1,
                                 unsigned long a2, unsigned long a3, unsigned long a4,
                                 unsigned long a5) {
  if (nr == 93 || nr == 94) census_dump();     // exit / exit_group: _exit() skips atexit()
  unsigned long long t0 = mach_absolute_time();
  long r = route_syscall(nr, a0, a1, a2, a3, a4, a5);
  unsigned long long d = mach_absolute_time() - t0;
  if (nr < CENSUS_MAX) {
    __atomic_fetch_add((unsigned long long*)&g_cn[nr], 1ULL, __ATOMIC_RELAXED);
    __atomic_fetch_add((unsigned long long*)&g_ct[nr], d, __ATOMIC_RELAXED);
  } else {
    __atomic_fetch_add(&g_cn_other, 1ULL, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_ct_other, d, __ATOMIC_RELAXED);
  }
  return r;
}
// ---- end P2 instrument -----------------------------------------------------------------------

static long route_syscall(unsigned long nr, unsigned long a0, unsigned long a1, unsigned long a2,
                          unsigned long a3, unsigned long a4, unsigned long a5) {
  // exit / exit_group. The guest's exit becomes a host _exit(), which skips atexit(), so the W^X
  // final counts have to be printed HERE or they are never printed at all -- and they are the only
  // way to see the emulation THRASHING, which is this design's failure mode. (census_dump() had to
  // solve exactly this, at line 139, but only on the instrumented route.)
  if (nr == 93 || nr == 94) gmem_wx_report();
  if (g_sc_enter < 0) g_sc_enter = getenv("RDSCENTER") != 0;
  if (g_sc_enter) {
    fprintf(stderr, "[sc>] tid=%d nr=%lu(%#lx,%#lx,%#lx)\n", guest_tid(), nr, a0, a1, a2);
    fflush(stderr);
  }
  if (nr == GPU_SYSCALL)          return gpu_hypercall((struct gpu_req*)a0);
  if (nr == HOSTCALL_SYSCALL)     return hostcall((struct hostcall_req*)a0);
  if (net_owns_syscall(nr))       return net_syscall(nr, a0, a1, a2, a3, a4, a5);
  return linux_syscall(nr, a0, a1, a2, a3, a4, a5);
}

// ---------------------------------------------------------------------------
// guest threads
// ---------------------------------------------------------------------------
// Two things make this awkward on macOS, and both are structural:
//
// 1. The guest thread pointer cannot live in a single global any more. TPIDR_EL0 is not usable
//    (macOS owns it, and on Apple Silicon it reads 0x2008 on EVERY thread — it is not a
//    per-thread pointer at all), so the value is kept here, keyed by the faulting thread's MACH
//    PORT. The trap handler runs on the exception-server thread, not the guest thread, so a
//    _Thread_local would be the wrong thread's copy; the port is the only identity available.
//
// 2. Syscalls are serviced on the exception-server thread, and mach_msg_server handles ONE
//    message at a time. A guest thread blocking in futex_wait would therefore block every other
//    guest thread's syscalls, including the futex_wake meant to release it — an immediate
//    deadlock. So each guest thread gets its own exception-server thread.
static mach_port_t g_port;      // this process's exception port (re-made in a fork child)

// RDSIGTRAP=1: service `brk` with an IN-THREAD BSD SIGTRAP handler instead of a Mach exception.
// The Mach path is a cross-thread round trip -- the kernel has to marshal a message, wake the
// exception-server thread, run mach_msg_server, and push a whole new thread state back -- and
// MEASURED on this machine that costs ~15us per trap with an EMPTY handler, against ~2.6us for
// the same brk delivered as a signal to the thread that took it. The handler body is not the
// cost and never was: the two hashed trap_by() lookups it performs total ~4ns.
static int g_sigtrap_mode;
static void sigtrap_thread_setup(void);   // per-guest-thread: alt stack + unblock SIGTRAP
static exception_mask_t exc_mask(void);   // BREAKPOINT only when the Mach path is in use

// ONE RECORD PER LIVE GUEST THREAD, AND RUNNING OUT IS A CORRECTNESS FAILURE, NOT A CAPACITY ONE.
// A guest thread with no record has no x18 home the trap handler can reach (see gt_for and the
// note in service_brk): its trapped x18 sites fall back to the process-global word while its
// veneered sites keep using its per-thread TSD slot, so a value written through one is invisible
// to the other. That is silent — x18 being wrong corrupts values, it does not trap — and libcrypto
// and libgnutls are exactly where silent value corruption is least acceptable.
//
// 128 was reached by MEASUREMENT, not in theory: tests/wl/x18many.c at 140 live guest threads fills
// the table, and a heavily-threaded browser is the normal case rather than the extreme one. The
// array is BSS and one record is well under 100 bytes, so the cap costs ~100 KB at 1024.
#define MAX_GTHREAD 1024

struct gthread {
  int           used;
  mach_port_t   port;            // mach thread port: the identity the trap handler sees
  unsigned long tpidr;           // this thread's virtualized TPIDR_EL0
  int           tid;             // guest tid
  unsigned long ctid;            // CLONE_CHILD_CLEARTID address, 0 if none
  // THE ONE HOME for this thread's guest x18 — the very word the VENEERS address inline
  // (mrs xJ,tpidrro_el0 / and xJ,xJ,#~7 / ldr|str xK,[xJ,#tsd_off]), recorded here as an address
  // so the TRAP path can reach the same word. See the invariant note above gmem_x18_slot() and
  // gt_bind_x18() below.
  //
  // This used to be `unsigned long x18` — a SECOND, unsynchronised home. That was safe only
  // because x18 neutralisation was all-or-nothing per image, so within any one image every site
  // agreed on which home it meant. Storing the address instead removes that dependency: veneered
  // and trapped sites now touch the same memory even inside one image.
  //
  // NULL only for a thread that has never bound its slot; the handler then falls back to
  // gmem_guest_x18, which is also exactly what gmem_x18_slot() returns when TSD storage is off.
  unsigned long* x18_slot;
  // THE ONE HOME for this thread's guest thread-pointer, for exactly the same reason and with
  // exactly the same rule: `mrs xN, tpidr_el0` is veneered inline against this thread's TSD slot,
  // and the trap handler (which still services the WRITE, and any read that could not be
  // veneered) must reach the same word rather than a copy. Recorded by the guest thread itself in
  // gt_bind_x18, because gmem_tls_slot() reads TPIDRRO_EL0 and so only names the right slot on
  // the thread it belongs to. NULL for a thread that never bound one; the handler then uses
  // gthread::tpidr, which is also the only home in that configuration.
  unsigned long* tpidr_slot;
  unsigned long long x18_saved;  // scratch reg stashed across ONE trap-path x18 instruction.
                                 // Per-thread by necessity: it was per-site, and threads then
                                 // clobbered each other's stash between enter and leave.
  int           vfork_slot;      // 1-based index into g_vf, or 0
  int           is_child;        // created by clone(): exit() must end the thread, not the process
};
static struct gthread   g_gt[MAX_GTHREAD];
static pthread_mutex_t  g_gt_mu = PTHREAD_MUTEX_INITIALIZER;
static int              g_next_tid = 1;
static int              g_gt_peak;      // high-water mark of live records; see gt_for()
int  guest_gthread_peak(void) { return g_gt_peak; }

// Find (or create) the record for a mach thread port. Called from the trap handler.
static struct gthread* gt_for(mach_port_t th) {
  pthread_mutex_lock(&g_gt_mu);
  struct gthread* r = NULL;
  for (int i = 0; i < MAX_GTHREAD; i++)
    if (g_gt[i].used && g_gt[i].port == th) { r = &g_gt[i]; break; }
  if (!r) {
    for (int i = 0; i < MAX_GTHREAD; i++)
      if (!g_gt[i].used) {
        memset(&g_gt[i], 0, sizeof g_gt[i]);
        g_gt[i].used = 1; g_gt[i].port = th; g_gt[i].tid = g_next_tid++;
        r = &g_gt[i];
        break;
      }
    // RECLAIM RECORDS WHOSE THREAD IS GONE, rather than degrade. A record is only released by
    // guest_thread_exiting(), which needs the guest thread to reach the exit syscall with t_cur
    // set — so a thread that dies any other way (its host pthread returns, the guest longjmps out,
    // a vfork child exits) leaks its record forever and the table fills with the dead. Asking Mach
    // whether the thread still exists is the only honest test, and a recycled port name is safe
    // here because gt_for() matches on the name first and gt_bind_x18() rebinds unconditionally.
    if (!r) {
      for (int i = 0; i < MAX_GTHREAD; i++) {
        if (!g_gt[i].used) continue;
        struct thread_basic_info bi; mach_msg_type_number_t cnt = THREAD_BASIC_INFO_COUNT;
        if (thread_info(g_gt[i].port, THREAD_BASIC_INFO, (thread_info_t)&bi, &cnt) == KERN_SUCCESS)
          continue;                                  // still alive: leave it alone
        memset(&g_gt[i], 0, sizeof g_gt[i]);
        g_gt[i].used = 1; g_gt[i].port = th; g_gt[i].tid = g_next_tid++;
        r = &g_gt[i];
        fprintf(stderr, "[shim] gthread table full; reclaimed the record of a dead thread\n");
        break;
      }
    }
    // PEAK live records, reported at exit. The question "is MAX_GTHREAD ever actually reached by a
    // real guest?" decides whether the exhaustion path below is theoretical or load-bearing, and
    // nothing in the shim could answer it before.
    { int live = 0;
      for (int i = 0; i < MAX_GTHREAD; i++) if (g_gt[i].used) live++;
      if (live > g_gt_peak) g_gt_peak = live; }
  }
  // EXHAUSTION IS NOT BENIGN AND IT USED TO BE SILENT. Returning NULL costs the caller its x18 home
  // (gt_bind_x18 seeds this thread's TSD slot but has nowhere to record its ADDRESS), so the trap
  // handler falls back to the shared global while this thread's veneers keep using the TSD slot --
  // a split home, per the note in service_brk. A guest with more than MAX_GTHREAD live threads is
  // exactly a heavily-threaded browser, so say so rather than corrupting quietly.
  // STILL FULL, AND THERE IS NO CORRECT WAY TO CONTINUE. Returning NULL used to be silent, and what
  // it bought was a guest that keeps running while computing on an x18 that two mechanisms disagree
  // about. Wrong crypto that reports success is worse than a stop with a diagnostic, so say what
  // happened and stop. Recovering means raising MAX_GTHREAD, not tolerating this.
  if (!r) {
    fprintf(stderr, "[shim] gthread table FULL: %d live guest threads and none reclaimable. A "
                    "thread with no record has NO x18 home the trap handler can reach, so its "
                    "trapped x18 sites would use the shared global while its veneers use per-thread "
                    "TSD — silent value corruption. Raise MAX_GTHREAD.\n", MAX_GTHREAD);
    fflush(stderr);
    pthread_mutex_unlock(&g_gt_mu);
    _exit(44);
  }
  pthread_mutex_unlock(&g_gt_mu);
  return r;
}

// Give this guest thread its x18 home and put `v` in it.
//
// MUST be called ON the guest thread itself, before that thread can execute any guest code:
// gmem_x18_slot() reads TPIDRRO_EL0, so it only names the right slot on the thread it belongs to.
// Every guest thread goes through exactly one of these — guest_thread (the main one),
// child_trampoline (clone), forked_guest (fork), vfork_trampoline (vfork) — and each of them is
// the first thing that runs on a brand new pthread, whose TSD slots are zero.
//
// Seeding is not optional: a fresh pthread's TSD slot is 0, nothing but an emitted veneer ever
// writes it, and a veneered `blr x18` through 0 branches to null.
//
// The assignment is UNCONDITIONAL on purpose. Mach thread ports get recycled, so gt_for() can
// hand back a record left behind by a guest thread that died without unhooking itself — and a
// stale slot address points into a freed TSD block, which is a wild write rather than merely a
// wrong read. Every guest thread rebinds here before it runs a single guest instruction, so a
// recycled record is always refreshed before the trap handler can dereference it.
static void gt_bind_x18(struct gthread* g, unsigned long v) {
  gmem_x18_seed(v);                      // writes THIS thread's slot (or the global, if TSD off)
  if (g) g->x18_slot = gmem_x18_slot();
  // The thread pointer's slot is bound on exactly the same footing, from the value the caller has
  // already placed in gthread::tpidr (clone's CLONE_SETTLS value, fork/vfork's inherited one, or
  // 0 for the main thread before the guest sets it). Seeding is not optional for the same reason
  // it is not optional for x18: a fresh pthread's TSD slot is zero, and a veneered read would then
  // hand glibc a null thread pointer.
  gmem_tls_seed(g ? g->tpidr : 0);
  // NULL when there is no per-thread storage (RDTLSVENEER=0, or a failed pthread_key_create), and
  // it MUST be propagated as NULL: gt_tpidr_ptr() then uses gthread::tpidr, which is per-thread by
  // construction and is what the trap handler used before the veneer existed. Substituting any
  // global here is what made every guest thread share one thread pointer (P2; see gmem_tls_slot).
  if (g) g->tpidr_slot = gmem_tls_slot();
}

// This thread's ONE home for the guest thread pointer, as seen from the exception-server thread.
// Safe despite naming another thread's storage: the faulting thread is stopped for the duration.
static unsigned long* gt_tpidr_ptr(struct gthread* g) {
  if (g && g->tpidr_slot) return g->tpidr_slot;
  return g ? &g->tpidr : &g_guest_tpidr;
}
static unsigned long gt_tpidr_get(struct gthread* g) { return *gt_tpidr_ptr(g); }

// Read another thread's guest x18 from the exception-server thread.
//
// Safe despite naming a different thread's storage: the owning guest thread is STOPPED for the
// whole duration of the exception we are servicing, so there is no concurrent writer. What is
// NOT safe from here is gmem_x18_slot() — TPIDRRO_EL0 read on the exception thread names the
// exception thread. Dereferencing an address that the right thread recorded earlier is the
// whole trick.
static unsigned long gt_x18_get(struct gthread* g) {
  return (g && g->x18_slot) ? *g->x18_slot : gmem_guest_x18;
}

// The record for the thread currently in a syscall. Set by the trap handler before dispatch so
// syscalls.c can ask "which guest thread am I?" without threading a parameter through everything.
static _Thread_local struct gthread* t_cur;

int  guest_tid(void)        { return t_cur ? t_cur->tid : 1; }

// Is `tid` one of OUR guest threads? The signal path needs this because guest tids and host pids
// are DIFFERENT NAMESPACES that happen to overlap at small integers -- guest tids start at 1, and
// host pid 1 is launchd. Passing a guest tid to the host kill() is not a wrong target, it is a
// wrong universe.
int guest_tid_known(int tid) {
  if (tid <= 0) return 0;
  pthread_mutex_lock(&g_gt_mu);
  int found = 0;
  for (int i = 0; i < MAX_GTHREAD; i++)
    if (g_gt[i].used && g_gt[i].tid == tid) { found = 1; break; }
  pthread_mutex_unlock(&g_gt_mu);
  return found || tid == 1;          // tid 1 is the main guest thread even before registration
}
// Host children we spawned for the guest (see guestthread.h). A small ring is enough: it only has
// to answer "did we make this pid", and a guest that outlives 256 children has long since lost
// interest in the first of them. Reaped pids are deliberately NOT removed — a guest may signal a
// zombie it has not waited for yet, and kill() on a dead pid is a harmless ESRCH from the host.
#define MAX_GCHILD 256
static int             g_gchild[MAX_GCHILD];
static int             g_gchild_n;
static pthread_mutex_t g_gchild_mu = PTHREAD_MUTEX_INITIALIZER;

void guest_child_register(int pid) {
  if (pid <= 0) return;
  pthread_mutex_lock(&g_gchild_mu);
  g_gchild[g_gchild_n++ % MAX_GCHILD] = pid;
  if (g_gchild_n > 2 * MAX_GCHILD) g_gchild_n -= MAX_GCHILD;   // keep the counter bounded
  pthread_mutex_unlock(&g_gchild_mu);
}

int guest_child_known(int pid) {
  if (pid <= 0) return 0;
  pthread_mutex_lock(&g_gchild_mu);
  int found = 0;
  for (int i = 0; i < MAX_GCHILD; i++) if (g_gchild[i] == pid) { found = 1; break; }
  pthread_mutex_unlock(&g_gchild_mu);
  return found;
}

int  guest_is_child(void)   { return t_cur ? t_cur->is_child : 0; }
void guest_set_tid_address(unsigned long addr) { if (t_cur) t_cur->ctid = addr; }

// A guest thread has called exit(). Publish the clear-tid word and wake anyone joining, exactly
// as CLONE_CHILD_CLEARTID promises, then unhook the record.
void guest_thread_exiting(void) {
  struct gthread* g = t_cur;
  if (!g) return;
  if (g->ctid) {
    *(volatile int*)g->ctid = 0;
    linux_syscall(98 /* futex */, g->ctid, 1 /* FUTEX_WAKE */, 1, 0, 0, 0);
  }
  pthread_mutex_lock(&g_gt_mu);
  g->used = 0;
  pthread_mutex_unlock(&g_gt_mu);
  t_cur = NULL;
}



// The CTR_EL0 the guest sees. Derived from the host's real cache line size so __clear_cache
// walks it in the right stride. Under-reporting the line is always SAFE (more iterations, every
// real line still touched); over-reporting would skip lines and leave stale icache, so the
// fallback is deliberately tiny.
static unsigned long ctr_el0_value(void) {
  static unsigned long cached;
  if (cached) return cached;
  long line = 0;
  size_t len = sizeof line;
  if (sysctlbyname("hw.cachelinesize", &line, &len, NULL, 0) != 0 || line < 4) line = 16;
  unsigned lg = 0;                                   // log2(line / 4), i.e. line size in WORDS
  while ((4u << lg) < (unsigned long)line && lg < 15) lg++;
  cached = 0x80000000UL                              // bit31 RES1
         | ((unsigned long)lg << 16)                 // DminLine
         | (3UL << 14)                               // L1Ip = PIPT
         | ((unsigned long)lg << 24)                 // CWG
         | ((unsigned long)lg << 20)                 // ERG
         | (unsigned long)lg;                        // IminLine
  fprintf(stderr, "[shim] CTR_EL0 synthesised as %#lx (host cache line %ld bytes)\n", cached, line);
  return cached;
}

// --- guest path -> host path ------------------------------------------------------------------
// execve was the ONLY caller in this project that resolved guest symlinks, via a private copy of
// the logic here. That copy is gone: syscalls.c now owns the one resolver (syscalls_resolve), and
// open/openat/stat/access/... go through it too. Keeping a second copy is what let the bug live —
// the fix had to land twice and only ever landed here, so every busybox applet ran while
// `cat /bin/cat` could not open an absolute symlink. The old copy also only readlink()ed the FULL
// path, so it never handled a symlink in a DIRECTORY component.
static int guest_host_path(const char* rootfs, const char* gpath, char* out, size_t cap) {
  (void)rootfs;                       // the resolver already holds the root (syscalls_set_root)
  return syscalls_resolve(-100 /*AT_FDCWD*/, gpath && gpath[0] ? gpath : "/", 0,
                          out, cap, 0, 0) < 0 ? -1 : 0;
}

// --- execve ---------------------------------------------------------------------------------
// Re-exec THIS binary as   <shim> <rootfs> <guest-path> [args...]   so a brand new shim loads the
// new program. The guest's own environment is handed over verbatim through the host environment,
// marked with RDGUESTENV so build_guest_env() knows to pass it straight through instead of
// synthesising the usual defaults.
static char g_self_path[1024];
static char g_rootfs_path[1024];
#define GPATH_ENV_MAX (4096 + 16)   // "RDGUESTCWD=" + a GPATH_MAX guest path

// Private diagnostic for the AT-SPI nested daemon's --print-address 13 pipe.
static void idle_trace_fd13(const char* phase, const char* path) {
  if (!getenv("IDLE_FD13_TRACE") || !path || !strstr(path, "dbus-daemon")) return;
  int flags = fcntl(13, F_GETFD);
  struct stat st = {0};
  int ok = fstat(13, &st);
  fprintf(stderr, "[idle-fd13] phase=%s pid=%ld path=%s fd13=%s cloexec=%d errno=%d\n",
          phase, (long)getpid(), path,
          ok == 0 ? (S_ISFIFO(st.st_mode) ? "pipe" :
                     S_ISSOCK(st.st_mode) ? "socket" : "other") : "missing",
          flags >= 0 ? !!(flags & FD_CLOEXEC) : -1, ok == 0 ? 0 : errno);
}

long guest_execve(const char* guest_path, char* const* argv, char* const* envp) {
  idle_trace_fd13("before-guest-exec", guest_path);
  if (!g_self_path[0] || !g_rootfs_path[0]) return -38;        // -ENOSYS: never recorded
  // The path must exist in the rootfs, or the exec'd shim just fails later with a worse message.
  char host[2048];
  guest_host_path(g_rootfs_path, guest_path, host, sizeof host);
  if (access(host, X_OK) != 0) return -2;                      // -ENOENT

  int gargc = 0; if (argv) while (argv[gargc]) gargc++;
  char** hargv = (char**)calloc((size_t)gargc + 4, sizeof *hargv);
  if (!hargv) return -12;
  int n = 0;
  hargv[n++] = g_self_path;
  hargv[n++] = g_rootfs_path;
  hargv[n++] = (char*)guest_path;
  for (int i = 1; i < gargc; i++) hargv[n++] = argv[i];        // argv[0] is the program name
  hargv[n] = NULL;

  int genvc = 0; if (envp) while (envp[genvc]) genvc++;
  // Carry the SHIM's own control variables across the exec as well. They are host-side settings
  // read by the new shim itself, not part of the guest environment, so without this an exec'd
  // child is silently untraceable — which is exactly how WebKit's JIT stayed invisible: it runs
  // in the exec'd WebKitWebProcess, and only the parent was ever being traced.
  static const char* kCarry[] = {
    // EVERY RDX18_* knob must ride across execve or an A/B silently reverts in the child — and
    // the interesting guests (Qt, WebKit) all exec, so the child is usually the part under test.
    // Only RDX18_NOMT was here; NOTSD, NOPLAIN and MIXN were not, so e.g.
    // "RDX18_NOTSD=1 ./shim_gui ... qmlscene" measured the DEFAULT and looked like a null result.
    // (This is about the shim's own MODE. The guest's x18 VALUE deliberately does not survive
    // execve — a new program image gets a new x18 by definition.)
    "RDVERBOSE", "RDNOVDSO", "RDFASTCALL", "RDPOLLLOG", "RDUID0",
    // RDDRM gates the rdmetal DRM render node (drmasahi.c). It MUST ride across execve for the
    // same reason RDSOCKETGROUP does: a compositor launched by startplasma-wayland (kwin_wayland)
    // is a re-exec'd shim, and without the node it reports "Failed to create gbm device for
    // /dev/dri/renderD128" and never brings up its EGL scene — which looks exactly like a client
    // that cannot reach the compositor. The top-level invocation worked only because RDDRM was in
    // the launch environment; the exec'd child got a fresh environment without it.
    "RDDRM", "RDDRM_TRACE",
    "RDX18_NOMT", "RDX18_NOTSD", "RDX18_NOPLAIN", "RDX18_MIXN", "RDX18_PARTIAL",
    "RDX18_BRPERSITE",
    "X11SRV_DEBUG", "X11SRV_DUMP", "WLSRV_DEBUG", "WLSRV_DUMP", "WLSRV_DUMP_MAX",
    "RDINPUT_SELFTEST", "RDINPUT_SELFTEST_VK", "RDINPUT_SELFTEST_DELAY",
    "RDINPUT_SELFTEST_MOUSE", "RDINPUT_SELFTEST_MOUSE_DELAY",
    "RDINPUT_SELFTEST_MOUSE_STEP_MS", "RDINPUT_SELFTEST_MOUSE_VCURSOR",
    // The cursor-injection knobs were the ones still missing, and they fail the same silent way:
    // the six RDINPUT_SELFTEST_* names above ride across, so a scripted run keeps its script in
    // an exec'd child while the fake cursor feeding the same poll quietly reverts to the physical
    // mouse. Qt and WebKit both exec, so the child is usually the process under test.
    "RDINPUT_FAKECURSOR", "RDINPUT_FAKECURSOR_MS",
    "RDWLH_CURSOR", "RDWLH_CURSOR_MS", "RDWLH_CLICK", "RDWLH_CLICKS",
    // NOT RDUNIXPREFIX, deliberately: the child re-runs wlsrv_start(), which unlink()s before
    // bind(), so a child inheriting the parent's prefix would steal the parent's compositor
    // socket — the exact three-instances-on-one-path failure the per-process default was added
    // to stop. The child getting its own namespace is correct here.
    // RDSEQPACKET must ride across execve for the same reason every RDX18_* knob does: WebKit's
    // socketpair is created by the UI process and USED by the exec'd WebKitWebProcess, so a child
    // that reverted to the default would make the A/B measure nothing.
    "RDSEQPACKET", "RDCREDLOG", "RDSOCKETGROUP", "RDSHARE",
    "RDNETSOCK",
    "RDINPUT_TRACE", "RDX_MEMFD_DIR",
    // RDFUTEX is the futex trace, and the process that hangs is the EXEC'D WebKitWebProcess, so
    // without this the only process traced is the one that is not stuck.
    "RDFUTEX", "RDFUTEX_SC",
    // RDWXNO turns fault-driven W^X emulation OFF, i.e. it is the negative-control arm for the
    // fix that gives JavaScriptCore a JIT. It MUST ride across execve, because the process whose
    // JIT decides whether a page loads is the exec'd WebKitWebProcess — a control arm that
    // silently reverted in the child would measure the treatment and look like a null result.
    // This is the trap that cost two earlier agents every arm they ran.
    "RDWXNO",
    // RDHOSTCALL gates the host-call bridge the forwarding Vulkan ICD needs to dlopen MoltenVK,
    // and it was NOT here -- so it died at execve in exactly the process that draws the page.
    // MEASURED: a MiniBrowser run launched with RDHOSTCALL=1 logs, from the exec'd children,
    //     [b7icd] cannot dlopen host /opt/homebrew/lib/libMoltenVK.dylib - is RDHOSTCALL=1 set?
    //     MESA: error: ZINK: vkCreateInstance failed (VK_ERROR_INCOMPATIBLE_DRIVER)
    // eight times over, while the top-level process reports the flag set. Same silent-revert-in-
    // the-child shape as every RDX18_* knob above; glmark2 never caught it because glmark2 does
    // not exec and WebKit always does.
    "RDHOSTCALL", 0
  };
  int ncarry = 0;
  for (int i = 0; kCarry[i]; i++) if (getenv(kCarry[i])) ncarry++;

  // The CWD. On Linux it survives execve; here execve is a fresh shim process whose g_cwd starts at
  // "/", so without this an exec'd program resolves every relative path against the rootfs ROOT and
  // succeeds at doing the wrong thing. Carried as a host-side RD* variable, which build_guest_env()
  // already strips from the guest's own environment, so the guest never sees it.
  char cwdenv[GPATH_ENV_MAX];
  snprintf(cwdenv, sizeof cwdenv, "RDGUESTCWD=%s", syscalls_get_cwd());

  char** henv = (char**)calloc((size_t)genvc + ncarry + 3, sizeof *henv);
  if (!henv) { free(hargv); return -12; }
  int m = 0;
  henv[m++] = (char*)"RDGUESTENV=1";
  henv[m++] = cwdenv;
  for (int i = 0; i < genvc; i++) henv[m++] = envp[i];
  for (int i = 0; kCarry[i]; i++) {
    const char* v = getenv(kCarry[i]);
    if (!v) continue;
    size_t n = strlen(kCarry[i]) + strlen(v) + 2;
    char* kv = (char*)malloc(n);
    if (!kv) continue;
    snprintf(kv, n, "%s=%s", kCarry[i], v);
    henv[m++] = kv;
  }
  henv[m] = NULL;

  fflush(NULL);
  if (guest_is_vfork_child()) {
    if (getenv("RDVERBOSE")) fprintf(stderr, "[vfork] child: execve %s\n", guest_path);
    // Replacing THIS process would take the parent with us — we share its address space. Spawn a
    // separate host shim and hand the pid to the parent that is blocked waiting for exactly this.
    pid_t np = -1;
    int rc = posix_spawn(&np, g_self_path, NULL, NULL, hargv, henv);
    if (rc == 0) {
      guest_child_register((int)np);              // the guest may legitimately kill() this pid
      net_note_spawned_child((int)np);            // ...and it is now the peer of our socketpairs
    }
    free(hargv); free(henv);
    guest_vfork_finish(rc == 0 ? (long)np : -1, rc == 0 ? 0 : 127);   // never returns
    return -8;
  }
  execve(g_self_path, hargv, henv);
  free(hargv); free(henv);
  return -8;                                                   // -ENOEXEC
}

// A child that _exit()s without exec'ing still has to be waitpid()-able by its parent, and the
// parent is expecting a real pid. Spawn a trivial host process that carries the same status.
static long spawn_status_carrier(int status) {
  char code[32];
  snprintf(code, sizeof code, "exit %d", status & 0xFF);
  char* av[] = { (char*)"/bin/sh", (char*)"-c", code, NULL };
  pid_t np = -1;
  if (posix_spawn(&np, "/bin/sh", NULL, NULL, av, NULL) != 0) return -11;
  guest_child_register((int)np);
  return np;
}



// CLONE_VM shares memory but NOT the file-descriptor table — a vfork child gets a COPY. Running
// the child as a thread gets memory right and fds wrong, and the consequence is loud: popen()'s
// child does dup2(pipe, 1), which in a shared table redirects the PARENT's stdout into the pipe
// and everything it prints afterwards vanishes.
//
// So snapshot the descriptors around the child: stash a duplicate of every open fd, let the child
// rearrange the real ones (the spawned process then inherits exactly the arrangement it built),
// and put the parent's back afterwards. Racy against other guest threads using fds concurrently,
// but a posix_spawn child's pre-exec window is a handful of syscalls.
#define FDSNAP_MAX 64
#define FDSNAP_BASE 300
struct fdsnap { int orig[FDSNAP_MAX], save[FDSNAP_MAX]; int n; };

static void fd_snapshot(struct fdsnap* s) {
  s->n = 0;
  for (int fd = 0; fd < FDSNAP_MAX && s->n < FDSNAP_MAX; fd++) {
    if (fcntl(fd, F_GETFD) < 0) continue;                    // not open
    int dup = fcntl(fd, F_DUPFD_CLOEXEC, FDSNAP_BASE);
    if (dup < 0) continue;
    s->orig[s->n] = fd; s->save[s->n] = dup; s->n++;
  }
}
static void fd_restore(struct fdsnap* s) {
  for (int i = 0; i < s->n; i++) {
    if (s->save[i] != s->orig[i]) dup2(s->save[i], s->orig[i]);
    close(s->save[i]);
  }
  s->n = 0;
}

// --- CLONE_VFORK ----------------------------------------------------------------------------
// posix_spawn() (and therefore popen(), system(), and glib's g_spawn) is clone(CLONE_VM |
// CLONE_VFORK): the child SHARES the parent's memory and the parent blocks until the child execs
// or exits. Sharing memory is exactly what a thread does, so the child runs as one — this is a
// faithful implementation rather than an approximation. What the child must not do is re-exec the
// shim, because that would replace the parent as well; it spawns a separate host process instead
// and hands the pid back to the parent it has been blocking.
#define MAX_VFORK 32
struct vfork_slot {
  int   used;
  int   done;
  long  pid;                     // host pid after exec, or -1 if it exited without exec'ing
  int   status;
  pthread_mutex_t mu;
  pthread_cond_t  cv;
};
static struct vfork_slot g_vf[MAX_VFORK];

int guest_is_vfork_child(void) { return t_cur && t_cur->vfork_slot; }

void guest_vfork_finish(long host_pid, int status) {
  int idx = t_cur ? t_cur->vfork_slot : 0;
  if (idx) {
    struct vfork_slot* v = &g_vf[idx - 1];
    pthread_mutex_lock(&v->mu);
    v->pid = host_pid; v->status = status; v->done = 1;
    pthread_cond_broadcast(&v->cv);
    pthread_mutex_unlock(&v->mu);
  }
  if (t_cur) { t_cur->used = 0; t_cur = NULL; }
  pthread_exit(NULL);
}

// --- clone ---------------------------------------------------------------------------------
#define LX_CLONE_VM       0x00000100
#define LX_CLONE_THREAD   0x00010000
#define LX_CLONE_SETTLS   0x00080000
#define LX_CLONE_PARENT_SETTID 0x00100000
#define LX_CLONE_CHILD_CLEARTID 0x00200000
#define LX_CLONE_CHILD_SETTID  0x01000000

struct child_start {
  unsigned long regs[GUEST_REGS_WORDS];
  unsigned long tls, ctid, x18;
  int           tid;
  int           settls;
};

static void* exc_loop(void*);          // defined below; one server per guest thread

static void* child_trampoline(void* arg) {
  fastcall_thread_init();   // per-thread native stack for the syscall fast path
  struct child_start* cs = (struct child_start*)arg;

  // Register this thread BEFORE it can trap, so its very first syscall finds its own TLS value
  // rather than allocating a fresh record with a zero thread pointer.
  mach_port_t self = mach_thread_self();
  struct gthread* g = gt_for(self);
  if (g) {
    g->tid = cs->tid;
    g->is_child = 1;
    g->ctid = cs->ctid;
    if (cs->settls) g->tpidr = cs->tls;
  }

  // Bind this new thread's single x18 home and put the cloning thread's value in it. Done here,
  // on the new thread itself, and before its exception server exists — so no trap can observe an
  // unbound slot.
  gt_bind_x18(g, cs->x18);

  // Its own exception server, so this thread blocking in a syscall cannot stall the others.
  pthread_t ext;
  if (pthread_create(&ext, NULL, exc_loop, NULL) == 0) pthread_detach(ext);

  pthread_jit_write_protect_np(1);          // guest text must be executable on this thread
  sigtrap_thread_setup();

  unsigned long regs[GUEST_REGS_WORDS];
  memcpy(regs, cs->regs, sizeof regs);
  free(cs);
  enter_guest_regs(regs);                    // never returns
}


// A guest thread's saved register file, used to resume the guest after a fork().
struct fork_ctx { unsigned long regs[GUEST_REGS_WORDS]; unsigned long x18; };

static void* forked_guest(void* arg) {
  fastcall_thread_init();   // per-thread native stack for the syscall fast path
  struct fork_ctx* fc = (struct fork_ctx*)arg;
  mach_port_t self = mach_thread_self();
  struct gthread* g = gt_for(self);
  if (g) { g->tid = 1; g->is_child = 0; g->tpidr = g_guest_tpidr; }
  // fork() kept only the calling thread — the EXCEPTION thread — so the guest thread that called
  // fork(), and its TSD block, do not exist in this child. This is a brand new pthread with a
  // brand new, all-zero TSD block, so the parent's x18 has to be re-seeded into it. (The
  // pthread_key itself is just an int and survives fork intact, so the slot offset the veneers
  // were compiled with is still the right one.)
  gt_bind_x18(g, fc->x18);
  pthread_jit_write_protect_np(1);
  sigtrap_thread_setup();
  unsigned long regs[GUEST_REGS_WORDS];
  memcpy(regs, fc->regs, sizeof regs);
  free(fc);
  enter_guest_regs(regs);                    // never returns
}

// ---------------------------------------------------------------------------
// display servers across fork()
// ---------------------------------------------------------------------------
// The X11 server and the Wayland compositor run IN THIS PROCESS, each on a thread of its own.
// fork() keeps only the calling thread. do_fork already knows this about the guest thread and
// the Mach port and rebuilds both — but the display threads are gone in the child too, and
// nothing rebuilt them. What the child inherits instead is a set of descriptors onto the
// PARENT's sockets and a pile of statics describing a compositor that is not running.
//
// A guest that daemonizes (fork, parent exits, child carries on) therefore ends up with a
// display connection nobody will ever answer. It blocks on its first round trip and stays there:
// alive, holding sockets, 0% CPU, no window, no error, no output. That signature — a process
// that looks fine and does nothing — is what made kate take an entire swarm to diagnose. See
// wlsrv_restart_after_fork().
//
// The namespace MUST be re-keyed to the child's pid. wlsrv_start()/x11srv_start() unlink before
// bind, so a child that reuses the inherited path steals the socket from its own parent — the
// exact failure c3e8a57 fixed for concurrent shims, walked back in through fork.
#if !defined(NO_GPU) || defined(WL_HEADLESS)
#define SHIM_HAS_DISPLAY 1
static int g_display_up;                  // this process started an in-process display server
#endif

static void display_restart_in_fork_child(void) {
#ifdef SHIM_HAS_DISPLAY
  if (!g_display_up) return;              // no display to lose; nothing to do

  char pfx[64], sock[256];
  snprintf(pfx, sizeof pfx, "/tmp/rdx-%d", (int)getpid());   // the CHILD's pid: never the parent's
  // mkdir -p by hand. This used to be system("mkdir -p ..."), which forks, execs a shell and
  // waitpid()s -- three things a fork child of a MULTITHREADED process must not assume it can
  // do. Two mkdir(2) calls need nothing but the kernel.
  mkdir(pfx, 0700);
  { char d[128]; snprintf(d, sizeof d, "%s/tmp", pfx); mkdir(d, 0700);
    snprintf(d, sizeof d, "%s/tmp/.X11-unix", pfx); mkdir(d, 0700); }
  net_set_unix_prefix(pfx);               // so the child's own connect() lands on its own sockets

  snprintf(sock, sizeof sock, "%s/tmp/wayland-0", pfx);
  int wl = wlsrv_restart_after_fork(sock);
#ifndef NO_GPU
  snprintf(sock, sizeof sock, "%s/tmp/.X11-unix/X0", pfx);
  int x11 = x11srv_restart_after_fork(sock);
#else
  int x11 = 0;
#endif
  // A child that cannot serve its display must NOT carry on. Blocking forever on a socket that
  // will never be answered is strictly worse than dying: it leaves an orphan that looks healthy.
  // Exiting lets connect() fail loudly, or the parent's wait() see a real status.
  if (wl != 0 && x11 != 0) {
    fprintf(stderr, "[shim] fork child %d: no display server could be started; exiting rather "
                    "than wedging on a socket nothing will answer\n", (int)getpid());
    fflush(stderr);
    _exit(127);
  }
  g_display_up = 1;
#endif
}

// fork(): duplicate the host process, then rebuild the guest inside the child.
static long do_fork(const arm_thread_state64_t* ts) {
  // The parent's TLS/x18 for the thread that called fork have to be published somewhere the
  // child can read them: after fork the child is a different process, and only THIS thread's
  // state survives, so t_cur's record is what the child's single guest thread inherits.
  unsigned long saved_tpidr = gt_tpidr_get(t_cur);   // the LIVE word, i.e. what a veneer wrote
  unsigned long saved_x18   = gt_x18_get(t_cur);   // the LIVE word, i.e. what the veneers wrote

  fflush(NULL);                              // do not duplicate buffered output into the child
  pid_t pid = fork();
  if (pid < 0) return -11;                   // -EAGAIN
  if (pid > 0) { guest_child_register((int)pid); net_note_spawned_child((int)pid); return pid; }

  // ---- child ----------------------------------------------------------------------------
  // FIRST, BEFORE ANY OTHER LINE: give this child its own stdio.
  //
  // fork() keeps only the calling thread. Every mutex another thread held at that instant is
  // orphaned in the child and can never be unlocked -- including the FILE lock inside stderr.
  // The very next thing this child does is display_restart_in_fork_child() -> wlsrv_start(),
  // whose first act is fprintf(stderr, "[wl] compositor listening on %s").
  //
  // MEASURED, not reasoned: MiniBrowser under shim_wlh forked once and the child then sat at
  // 0.0% CPU forever, never reaching execve. sample(1) on that child, pid 86796:
  //
  //     service_brk -> wlsrv_start + 2832 -> fprintf -> flockfile
  //       -> _pthread_mutex_firstfit_lock_slow -> __psynch_mutexwait     [1751/1751 samples]
  //
  // 1751 samples out of 1751 -- it is not slow, it is stopped. From the outside that is exactly
  // "the browser forked and the web process never appeared": a live parent, a live child, no
  // grandchild, no error, no output. Apple's libc does NOT reset FILE locks across fork (glibc
  // does), so this cannot be waited out. A heavily threaded guest like WebKit, whose threads are
  // writing to stderr constantly under RDVERBOSE, loses this race routinely.
  //
  // Assigning `stderr` is legal here: on Darwin it is `extern FILE *__stderrp`, an lvalue. The
  // old FILE is deliberately abandoned -- it is unusable, and free()ing it would touch the same
  // dead lock. fdopen() itself is safe: libmalloc registers atfork handlers, stdio does not.
  {
    FILE* e = fdopen(2, "w");
    if (e) { setvbuf(e, NULL, _IONBF, 0); stderr = e; }
    FILE* o = fdopen(1, "w");
    if (o) { setvbuf(o, NULL, _IONBF, 0); stdout = o; }
  }

  // Only this thread exists now, and it is inside a Mach exception handler whose port died with
  // the parent. Stand up a fresh port, become the child's exception server, and hand the guest
  // to a new thread with the parent's registers and x0 == 0.
  g_guest_tpidr = saved_tpidr;
  gmem_guest_x18 = saved_x18;
  memset(g_gt, 0, sizeof g_gt);
  g_next_tid = 1;
  t_cur = NULL;

  // Before the guest runs again: it may connect to its display on the very next instruction, and
  // the display it inherited belongs to a process that is about to exit.
  display_restart_in_fork_child();

  mach_port_t np = MACH_PORT_NULL;
  mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &np);
  mach_port_insert_right(mach_task_self(), np, np, MACH_MSG_TYPE_MAKE_SEND);
  task_set_exception_ports(mach_task_self(), exc_mask(), np,
                           EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES, ARM_THREAD_STATE64);
  g_port = np;

  struct fork_ctx* fc = (struct fork_ctx*)calloc(1, sizeof *fc);
  if (!fc) _exit(127);
  for (int i = 0; i < 31; i++) fc->regs[i] = (unsigned long)ts->__x[i];
  fc->regs[0]  = 0;                          // the child sees fork() return 0
  fc->regs[31] = (unsigned long)ts->__sp;
  fc->regs[32] = (unsigned long)ts->__pc + 4;
  fc->x18      = saved_x18;                  // forked_guest re-seeds it into its own TSD slot

  pthread_t gt;
  if (pthread_create(&gt, NULL, forked_guest, fc) != 0) _exit(127);
  pthread_detach(gt);

  // Serve this child's traps forever. Deliberately never returns: the handler we are nested
  // inside was called through the PARENT's port, and replying on it is neither possible nor
  // wanted.
  while (1) mach_msg_server(mach_exc_server, 4096, g_port, 0);
  _exit(127);
}


static void* vfork_trampoline(void* arg);
struct vfork_start { unsigned long regs[GUEST_REGS_WORDS]; int slot;
                     unsigned long tpidr, x18; };

// CLONE_VM without CLONE_THREAD: run the child on a thread (it shares memory, so that is exact)
// and block here until it execs or exits, as CLONE_VFORK requires.
static long do_vfork(const arm_thread_state64_t* ts, unsigned long stack) {
  int idx = -1;
  for (int i = 0; i < MAX_VFORK; i++) if (!g_vf[i].used) { idx = i; break; }
  if (idx < 0) return -11;                                                // -EAGAIN
  struct vfork_slot* v = &g_vf[idx];
  memset(v, 0, sizeof *v);
  v->used = 1; v->pid = -1;
  pthread_mutex_init(&v->mu, NULL);
  pthread_cond_init(&v->cv, NULL);

  struct vfork_start* vs = (struct vfork_start*)calloc(1, sizeof *vs);
  if (!vs) { v->used = 0; return -12; }
  for (int i = 0; i < 31; i++) vs->regs[i] = (unsigned long)ts->__x[i];
  vs->regs[0]  = 0;
  vs->regs[31] = stack ? stack : (unsigned long)ts->__sp;
  vs->regs[32] = (unsigned long)ts->__pc + 4;
  vs->slot     = idx + 1;
  // CLONE_VM means the child shares everything, INCLUDING the thread pointer — it is the calling
  // thread's TLS that must carry over, not the global fallback. Getting this wrong leaves the
  // child's TLS at 0 and its first errno write faults at TLS-164.
  vs->tpidr    = gt_tpidr_get(t_cur);      // the LIVE word, not a stale copy
  vs->x18      = gt_x18_get(t_cur);        // ditto for x18: the LIVE word, not a stale copy

  struct fdsnap snap;
  fd_snapshot(&snap);

  if (g_verbose) fprintf(stderr, "[vfork] parent: starting child (slot %d, stack %#lx)\n",
                         idx, stack);
  pthread_t th;
  if (pthread_create(&th, NULL, vfork_trampoline, vs) != 0) {
    fd_restore(&snap); free(vs); v->used = 0; return -11;
  }
  pthread_detach(th);

  pthread_mutex_lock(&v->mu);
  while (!v->done) pthread_cond_wait(&v->cv, &v->mu);
  if (g_verbose) fprintf(stderr, "[vfork] parent: child finished (pid %ld)\n", v->pid);
  long pid = v->pid;
  int st = v->status;
  pthread_mutex_unlock(&v->mu);
  v->used = 0;
  fd_restore(&snap);                       // undo whatever the child did to our descriptors

  // The child exited without exec'ing. The parent is about to waitpid() on the pid we return, so
  // give it a real host process that carries the same exit code.
  if (pid < 0) pid = spawn_status_carrier(st);
  return pid;
}

static void* vfork_trampoline(void* arg) {
  fastcall_thread_init();   // per-thread native stack for the syscall fast path
  struct vfork_start* vs = (struct vfork_start*)arg;
  mach_port_t self = mach_thread_self();
  struct gthread* g = gt_for(self);
  if (g) {
    g->is_child = 1;
    g->vfork_slot = vs->slot;
    g->tpidr = vs->tpidr;
  }
  // Another brand new pthread with an all-zero TSD block. CLONE_VM means the child shares memory
  // with the parent, so it also inherits the parent's x18 — but not the parent's TSD slot, which
  // belongs to a different thread. Re-seed into this thread's own slot.
  gt_bind_x18(g, vs->x18);
  pthread_t ext;
  if (pthread_create(&ext, NULL, exc_loop, NULL) == 0) pthread_detach(ext);
  pthread_jit_write_protect_np(1);
  sigtrap_thread_setup();
  if (g_verbose) fprintf(stderr, "[vfork] child: entering guest at %#lx sp=%#lx\n",
                         vs->regs[32], vs->regs[31]);
  unsigned long regs[GUEST_REGS_WORDS];
  memcpy(regs, vs->regs, sizeof regs);
  free(vs);
  enter_guest_regs(regs);
}

// Serviced in the trap handler because it needs the FULL register file: the child resumes at the
// instruction after the `svc` with the parent's registers, its own stack, and x0 == 0.
//   x0 flags, x1 stack, x2 parent_tid, x3 tls, x4 child_tid
static long do_clone(const arm_thread_state64_t* ts) {
  unsigned long flags = (unsigned long)ts->__x[0];
  unsigned long stack = (unsigned long)ts->__x[1];
  unsigned long ptid  = (unsigned long)ts->__x[2];
  unsigned long tls   = (unsigned long)ts->__x[3];
  unsigned long ctid  = (unsigned long)ts->__x[4];

  // No CLONE_VM means fork(): a genuinely separate address space. That sounds impossible here —
  // guest pointers ARE host pointers — but it falls out for free, because a HOST fork() copies
  // this whole process, guest memory and all. The child's guest keeps every address it had, now
  // backed by copy-on-write pages, which is exactly fork's contract.
  //
  // The catch is that fork() keeps only the CALLING thread, and we are on the Mach
  // exception-server thread. So the child has no guest thread and no working exception port, and
  // has to rebuild both before it can run a single guest instruction.
  if (!(flags & LX_CLONE_VM)) return do_fork(ts);
  if (!(flags & LX_CLONE_THREAD)) return do_vfork(ts, stack);             // posix_spawn/popen

  // Historic: move every veneered x18 site to the trap path before a second guest thread exists.
  //
  // BY DEFAULT THIS NOW DOES NOTHING and returns immediately — a veneer already keeps x18 in
  // this thread's TSD slot, so a second guest thread needs nothing done to the rewritten code.
  // It still converts under RDX18_NOTSD=1, where the veneers share one global instead. Measured:
  // converting cost ~30x on threaded llvmpipe (osdemo 31-66s vs ~1.5s). See gmem_x18_multithread.
  //
  // The seed that used to sit here — `if (t_cur && !t_cur->x18) t_cur->x18 = gmem_guest_x18;` —
  // is GONE, and could never have been right: it primed the trap path's copy from the GLOBAL,
  // while the veneers had been writing this thread's TSD slot, so x18's live value was dropped at
  // the transition. It was harmless only because clone() is a call boundary, where x18 is
  // caller-saved and therefore dead. With one home there is nothing to prime: the sites this call
  // converts keep reading and writing the same word they did as veneers.
  gmem_x18_multithread();

  struct child_start* cs = (struct child_start*)calloc(1, sizeof *cs);
  if (!cs) return -12;                                                    // -ENOMEM

  for (int i = 0; i < 31; i++) cs->regs[i] = (unsigned long)ts->__x[i];
  cs->regs[0]  = 0;                                   // the child sees clone() return 0
  cs->regs[31] = stack;
  cs->regs[32] = (unsigned long)ts->__pc + 4;         // resume after the svc we trapped on
  cs->tls      = tls;
  cs->settls   = (flags & LX_CLONE_SETTLS) != 0;
  cs->ctid     = (flags & LX_CLONE_CHILD_CLEARTID) ? ctid : 0;
  cs->x18      = gt_x18_get(t_cur);       // the cloning thread's LIVE x18, veneer-written or not

  pthread_mutex_lock(&g_gt_mu);
  int tid = g_next_tid++;
  pthread_mutex_unlock(&g_gt_mu);
  cs->tid = tid;

  if ((flags & LX_CLONE_PARENT_SETTID) && ptid) *(volatile int*)ptid = tid;
  if ((flags & LX_CLONE_CHILD_SETTID)  && ctid) *(volatile int*)ctid = tid;

  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  // The HOST stack is never used by the guest (enter_guest_regs switches to the guest's own),
  // but the trampoline and the exception server need a little room before that happens.
  pthread_attr_setstacksize(&at, 256 * 1024);
  pthread_t th;
  int rc = pthread_create(&th, &at, child_trampoline, cs);
  pthread_attr_destroy(&at);
  if (rc != 0) { free(cs); return -11; }              // -EAGAIN
  return tid;
}

// --- trap counters ---------------------------------------------------------------------------
// RDTRAPSTAT=1 prints a per-second breakdown. The point is to know how much of a workload's wall
// clock is trap round-trips BEFORE optimising the handler body: on this machine one brk -> Mach
// exception -> server thread -> resume costs ~15us, and the whole handler body (two hashed
// trap_by() lookups) costs ~4ns. Anything that does not remove a ROUND TRIP is noise.
static volatile unsigned long long g_n_x18_enter, g_n_x18_leave, g_n_tls, g_n_ctr, g_n_sc;
static volatile unsigned long long g_n_tlsr, g_n_tlsw;   // P2: read/write split of g_n_tls
static volatile unsigned long long g_n_dczva, g_n_dczid; // dc zva / mrs DCZID_EL0
#define TRAPCNT(c) __atomic_fetch_add((unsigned long long*)&(c), 1ULL, __ATOMIC_RELAXED)

// P2 instrument: exact CUMULATIVE trap attribution at exit (RDTRAPSTAT's per-second lines
// sample, and always miss the tail second). Independent of the census counters above.
static void trapcnt_dump(void) {
  unsigned long long x18e = g_n_x18_enter, x18l = g_n_x18_leave,
                     tls = g_n_tls, ctr = g_n_ctr, sc = g_n_sc,
                     dczva = g_n_dczva, dczid = g_n_dczid;
  unsigned long long all = x18e + x18l + tls + ctr + sc + dczva + dczid;
  if (!all) all = 1;
  fprintf(stderr, "[trapcnt] TOTAL=%llu  tls=%llu (%.1f%%)  syscall=%llu (%.1f%%)  "
                  "[tls: read=%llu write=%llu]  x18_enter=%llu  x18_leave=%llu  ctr=%llu  "
                  "dc_zva=%llu  mrs_dczid=%llu\n",
          x18e + x18l + tls + ctr + sc + dczva + dczid, tls, 100.0 * tls / all, sc, 100.0 * sc / all,
          g_n_tlsr, g_n_tlsw, x18e, x18l, ctr, dczva, dczid);
  // Live-thread high-water mark against the table that has to hold one record per guest thread.
  // A guest that reaches MAX_GTHREAD loses its x18 home from that point on (see gt_for), so this
  // number is the margin on a correctness cliff, not a capacity statistic.
  fprintf(stderr, "[gthread] peak live records %d / %d\n", guest_gthread_peak(), MAX_GTHREAD);
  // W^X emulation cost, which is the price of having a JIT at all on this platform. exec_flips +
  // write_flips is the number of Mach exceptions the JIT cost us; a JIT whose pages settle into
  // R+X pays it once per page, and one that keeps patching pages it is also executing pays it
  // repeatedly (thrash). declined>0 with no [FAULT] cannot happen — a decline always falls through
  // to the fault report — so declined here is the count of genuine faults that reached the report.
  { unsigned long long xf, wf, dc, x18, pgs; int nr;
    gmem_wx_stats(&xf, &wf, &dc, &x18, &pgs, &nr);
    fprintf(stderr, "[wx] ranges=%d exec_flips=%llu write_flips=%llu declined=%llu  "
                    "x18_sites_in_generated_code=%llu over %llu page(s)\n", nr, xf, wf, dc, x18, pgs); }
  fflush(stderr);
}

static void* trapstat_loop(void* _) {
  (void)_;
  unsigned long long p[5] = {0};
  int tick = 0;
  for (;;) {
    struct timespec ts = { 1, 0 }; nanosleep(&ts, NULL);
    unsigned long long c[5] = { g_n_x18_enter, g_n_x18_leave, g_n_tls, g_n_ctr, g_n_sc };
    unsigned long long x18 = (c[0]-p[0]) + (c[1]-p[1]);
    unsigned long long all = x18 + (c[2]-p[2]) + (c[3]-p[3]) + (c[4]-p[4]);
    if (all)
      fprintf(stderr, "[trapstat] %llu traps/s  (x18 enter %llu, x18 leave %llu, tls %llu, "
                      "ctr %llu, syscall %llu)  total %llu\n",
              all, c[0]-p[0], c[1]-p[1], c[2]-p[2], c[3]-p[3], c[4]-p[4],
              c[0]+c[1]+c[2]+c[3]+c[4]);
    memcpy(p, c, sizeof p);
    if (++tick % 10 == 0) gmem_x18_trap_top(15);
  }
}

// Temporary diagnostics: dump every thread's PC/LR/SP (guest threads symbolised via gmem_image_for).
// Gated by RDTHREADDUMP_SECS=n: wait n seconds, then dump once (0 disables; default 20).
#include <dlfcn.h>
static void threaddump_sym(unsigned long long addr, char* out, size_t cap) {
  unsigned long gbase = 0;
  const char* img = gmem_image_for((unsigned long)addr, &gbase);
  if (img) { snprintf(out, cap, "%s+%#llx", img, addr - gbase); return; }
  Dl_info info;
  if (dladdr((void*)(uintptr_t)addr, &info) && info.dli_sname) {
    snprintf(out, cap, "%s+%#llx", info.dli_sname, addr - (unsigned long long)info.dli_saddr);
  } else if (dladdr((void*)(uintptr_t)addr, &info) && info.dli_fname) {
    const char* s = strrchr(info.dli_fname, '/');
    snprintf(out, cap, "%s+%#llx", s ? s + 1 : info.dli_fname,
             addr - (unsigned long long)info.dli_fbase);
  } else {
    snprintf(out, cap, "%#llx", addr);
  }
}

static void threaddump_once(void) {
  thread_array_t threads = NULL;
  mach_msg_type_number_t count = 0;
  if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
    fprintf(stderr, "[threaddump] task_threads failed\n");
    return;
  }
  fprintf(stderr, "[threaddump] ==== %u threads ====\n", count);
  for (mach_msg_type_number_t i = 0; i < count; i++) {
    arm_thread_state64_t ts;
    mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
    kern_return_t kr = thread_get_state(threads[i], ARM_THREAD_STATE64,
                                        (thread_state_t)&ts, &cnt);
    char name[64] = "?";
    pthread_t pt = pthread_from_mach_thread_np(threads[i]);
    if (pt) { char tmp[64]; if (pthread_getname_np(pt, tmp, sizeof tmp) == 0 && tmp[0]) {
        snprintf(name, sizeof name, "%s", tmp); } }
    if (kr != KERN_SUCCESS) {
      fprintf(stderr, "[threaddump]   t%-2u name=%s : thread_get_state failed kr=%d\n",
              i, name, kr);
      continue;
    }
    char pcs[160], lrs[160];
    threaddump_sym((unsigned long long)ts.__pc, pcs, sizeof pcs);
    threaddump_sym((unsigned long long)ts.__lr, lrs, sizeof lrs);
    fprintf(stderr, "[threaddump]   t%-2u name=%-20s pc=%#llx(%s) lr=%#llx(%s)\n",
            i, name, (unsigned long long)ts.__pc, pcs,
            (unsigned long long)ts.__lr, lrs);
    // walk the frame pointer chain
    unsigned long long fp = ts.__fp;
    int depth = 0;
    while (fp && depth < 32) {
      unsigned long long nxt = 0, ra = 0;
      unsigned long long* slot = (unsigned long long*)(uintptr_t)fp;
      nxt = slot[0]; ra = slot[1];
      if (!ra) break;
      char s[160]; threaddump_sym(ra, s, sizeof s);
      fprintf(stderr, "[threaddump]            #%-2d %s\n", depth, s);
      if (nxt <= fp) break;
      fp = nxt; depth++;
    }
  }
  if (threads) vm_deallocate(mach_task_self(), (vm_address_t)threads,
                             (vm_size_t)(count * sizeof(thread_t)));
  fflush(stderr);
}

static void* threaddump_loop(void* _) {
  (void)_;
  long secs = 20;
  { const char* e = getenv("RDTHREADDUMP_SECS"); if (e && atol(e) > 0) secs = atol(e); }
  struct timespec ts = { secs, 0 }; nanosleep(&ts, NULL);
  threaddump_once();
  return NULL;
}

// --- trap handler: brk -> syscall dispatch / TLS virtualization ---
// The body is factored out of the Mach handler so the SAME code can be reached two ways:
//   * a Mach EXC_BREAKPOINT message, serviced on a DIFFERENT thread (ts and ns are two buffers);
//   * a BSD SIGTRAP, serviced IN-THREAD on the faulting thread (ts and ns are the SAME buffer,
//     the signal's ucontext).
// Aliasing ts and ns is safe by inspection: every read of `ts` in here happens before the
// corresponding write to `ns` (the syscall arguments are read as call arguments, which are
// sequenced before the assignment to ns->__x[0]), and gmem_x18_trap_enter already reads and
// writes one and the same register array.
static void service_brk(arm_thread_state64_t* ts, arm_thread_state64_t* ns, struct gthread* gt);

kern_return_t catch_mach_exception_raise_state_identity(
    mach_port_t p, mach_port_t th, mach_port_t tk, exception_type_t e,
    mach_exception_data_t code, mach_msg_type_number_t cc, int* fl,
    thread_state_t os, mach_msg_type_number_t oc,
    thread_state_t ns_, mach_msg_type_number_t* nc) {
  (void)p; (void)tk; (void)fl;
  arm_thread_state64_t* ts = (arm_thread_state64_t*)os;
  memcpy(ns_, os, oc * sizeof(natural_t)); *nc = oc;
  arm_thread_state64_t* ns = (arm_thread_state64_t*)ns_;

  if (e != EXC_BREAKPOINT) {                        // fault diagnostics
    unsigned long pc = (unsigned long)ts->__pc, fault = (cc >= 2) ? (unsigned long)code[1] : 0;

    // FAULT-DRIVEN W^X, and this is where JavaScriptCore's JIT actually runs. A page in a guest
    // JIT region is R+W or R+X, never both, so the guest's next access to it faults and the fault
    // says which one it wants: an instruction-FETCH fault (the faulting address IS the pc, because
    // on arm64 a fetch fault reports the instruction's own address) means "make it executable"; a
    // data fault in the same region means "make it writable". Flip that one page and resume at the
    // same pc — the instruction re-executes and succeeds.
    //
    // Only EXC_BAD_ACCESS can be a protection fault; EXC_BAD_INSTRUCTION is a decoded opcode the
    // rewriter does not handle and flipping protections would turn it into an infinite loop.
    // gmem_wx_fault() declines anything outside a registered range, and declines a page that is
    // already in the state being asked for, so control falls through to the report below and the
    // process still dies on a genuine wild pointer.
    if (e == EXC_BAD_ACCESS && cc >= 2 && gmem_wx_fault(fault, fault == pc))
      return KERN_SUCCESS;

    unsigned int insn = (pc >= 0x100000000UL) ? *(unsigned int*)pc : 0;
    fprintf(stderr, "\n[FAULT] %s exc=%d fault_addr=%#lx pc=%#lx insn=%08x\n",
            e == EXC_BAD_INSTRUCTION ? "ILLEGAL INSTRUCTION (an opcode the rewriter does not "
                                       "handle, or one Darwin refuses at EL0)" : "bad access",
            e, fault, pc, insn);
    if (g_interp_base && pc >= g_interp_base)
      fprintf(stderr, "        pc = interp_base + %#lx\n", pc - g_interp_base);
    if (pc >= g_main_base && (g_interp_base == 0 || pc < g_interp_base))
      fprintf(stderr, "        pc = main_base + %#lx\n", pc - g_main_base);
    unsigned long ibase = 0;
    const char* iname = gmem_image_for(pc, &ibase);
    if (iname) fprintf(stderr, "        in %s + %#lx\n", iname, pc - ibase);
    // WHICH MAPPING IS THIS? "gmem_image_for returned NULL" has two very different meanings --
    // code in an image we simply do not track, or code in an ANONYMOUS mapping, i.e. JIT. The
    // Mach VM map answers it directly, and the protection bits say whether it went through the
    // W^X path. Printed for the faulting pc and for the address that faulted.
    {
      mach_vm_address_t a[2] = { (mach_vm_address_t)pc, (mach_vm_address_t)fault };
      const char* what[2] = { "pc", "fault_addr" };
      for (int q = 0; q < 2; q++) {
        mach_vm_address_t ra = a[q]; mach_vm_size_t rs = 0;
        vm_region_basic_info_data_64_t bi; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        kern_return_t kr = mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                                          (vm_region_info_t)&bi, &cnt, &obj);
        if (kr == KERN_SUCCESS && ra <= a[q] && a[q] < ra + rs)
          fprintf(stderr, "        %-10s region %#llx..%#llx prot=%c%c%c max=%c%c%c\n", what[q],
                  (unsigned long long)ra, (unsigned long long)(ra + rs),
                  (bi.protection & VM_PROT_READ) ? 'r' : '-',
                  (bi.protection & VM_PROT_WRITE) ? 'w' : '-',
                  (bi.protection & VM_PROT_EXECUTE) ? 'x' : '-',
                  (bi.max_protection & VM_PROT_READ) ? 'r' : '-',
                  (bi.max_protection & VM_PROT_WRITE) ? 'w' : '-',
                  (bi.max_protection & VM_PROT_EXECUTE) ? 'x' : '-');
        else
          fprintf(stderr, "        %-10s %#llx is in NO MAPPING\n", what[q],
                  (unsigned long long)a[q]);
      }
    }
    unsigned long lbase = 0;
    const char* lname = gmem_image_for((unsigned long)ts->__lr, &lbase);
    if (lname) fprintf(stderr, "        called from %s + %#lx\n", lname,
                       (unsigned long)ts->__lr - lbase);
    unsigned long vsite; unsigned int vinsn;
    if (gmem_veneer_lookup(pc, &vsite, &vinsn))
      fprintf(stderr, "        IN AN x18 VENEER for guest %#lx (original insn %08x)\n",
              vsite, vinsn);
    // A fault inside a trap STUB means the shadow we loaded into K was wrong. Report the
    // neighbouring trap records with their hit counts, so the reader can see whether the x18 WRITE
    // that should have produced the shadow ever ran (hits >= 1 -> the write-back was dropped) or
    // never did (hits == 0 -> control flow), or was never rewritten at all.
    gmem_x18_trap_dump_near(pc, 16, 4);
    // W^X state, printed on EVERY fault. A fault that gmem_wx_fault() DECLINED reaches here, and
    // without these counters "declined" is indistinguishable from "the range was never registered"
    // — which are different bugs (a wild pointer vs. a JIT region we failed to track).
    { unsigned long long xf, wf, dc, x18, pgs; int nr;
      gmem_wx_stats(&xf, &wf, &dc, &x18, &pgs, &nr);
      if (nr || xf || wf || dc)
        fprintf(stderr, "        wx: ranges=%d exec_flips=%llu write_flips=%llu declined=%llu "
                        "x18_sites_in_jit=%llu over %llu page(s)\n", nr, xf, wf, dc, x18, pgs); }
    // THE TWO REGISTERS THE FAULTING INSTRUCTION ACTUALLY USES, named rather than guessed.
    // A wild-pointer report that prints x0/x1/x2/x8 when the instruction is `ldr s27,[x4,x17]`
    // tells the reader nothing; decoding Rn/Rm from the opcode and printing THOSE is the whole
    // difference between "some pointer is bad" and "x17 is bad and here is what it holds".
    if (e == EXC_BAD_ACCESS && insn) {
      unsigned rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
      unsigned long long vn = rn == 31 ? (unsigned long long)ts->__sp : ts->__x[rn];
      unsigned long long vm = rm == 31 ? (unsigned long long)ts->__sp : ts->__x[rm];
      fprintf(stderr, "        operands: Rn=x%u=%#llx  Rm=x%u=%#llx  (Rn+Rm=%#llx)\n",
              rn, vn, rm, vm, vn + vm);
      for (unsigned i = 0; i < 31; i += 6) {
        fprintf(stderr, "        ");
        for (unsigned k = i; k < i + 6 && k < 31; k++)
          fprintf(stderr, "x%-2u=%#-18llx ", k, ts->__x[k]);
        fprintf(stderr, "\n");
      }
    }
    fprintf(stderr, "        x0=%#llx x1=%#llx x2=%#llx x8=%#llx lr=%#llx sp=%#llx\n",
            ts->__x[0], ts->__x[1], ts->__x[2], ts->__x[8], (unsigned long long)ts->__lr,
            (unsigned long long)ts->__sp);
    fflush(stderr); _exit(42);
  }

  // Identify the guest thread that trapped. The handler runs on the exception-server thread, so
  // the faulting thread's mach port is the only identity available here.
  service_brk(ts, ns, gt_for(th));
  return KERN_SUCCESS;
}

static void service_brk(arm_thread_state64_t* ts, arm_thread_state64_t* ns, struct gthread* gt) {
  unsigned int insn = *(unsigned int*)ts->__pc;     // the brk we planted
  unsigned int imm = (insn >> 5) & 0xFFFF;

  t_cur = gt;
  unsigned long* tpidr = gt_tpidr_ptr(gt);   // the same word the TLS veneers read, inline
  // P2 MUTATION KNOB, MEASUREMENT ONLY (RDTLS_SHARED=1): answer every thread's tpidr_el0 from ONE
  // global, i.e. deliberately reintroduce the exact bug an inline TLS veneer would ship if its
  // per-thread home were wrong. This is the NEGATIVE CONTROL for tests/wl/p2tls.c: with it set,
  // p2tls must FAIL "every thread's tpidr_el0 is DISTINCT". A gate that cannot be made to fail is
  // not a gate, and the x18 veneers shipped precisely this bug once (619k mismatches).
  { static int shared = -1;
    if (shared < 0) shared = (getenv("RDTLS_SHARED") != NULL);
    if (shared) tpidr = &g_guest_tpidr; }
  // THE SAME WORD THE VENEERS USE. gt->x18_slot was recorded by the faulting thread itself (see
  // gt_bind_x18); we cannot compute it here, because gmem_x18_slot() reads TPIDRRO_EL0 and this
  // is the exception-server thread. Dereferencing it is safe: the faulting thread is stopped.
  // The fallback is for a thread that never bound a slot, and matches what gmem_x18_slot()
  // returns when TSD storage is off — so both paths still agree.
  unsigned long* px18  = (gt && gt->x18_slot) ? gt->x18_slot : &gmem_guest_x18;
  // NEGATIVE-CONTROL INSTRUMENT for the ONE HOME invariant, and the fallback above is the branch it
  // watches. When per-thread storage is ON, every guest thread's VENEERS address its own TSD slot
  // inline -- so a thread reaching the fallback puts its TRAPPED sites on the process-global word
  // while its veneered sites keep using the TSD slot. That is a SPLIT HOME: x18 written under one
  // mechanism is invisible to the other, and it is silent, because a wrong x18 corrupts values
  // rather than trapping. The fallback is only correct when TSD storage is off, in which case
  // gmem_x18_slot() returns this same global and there is genuinely one home.
  // Gated on the traps that actually DEREFERENCE px18. Every brk comes through here — syscalls
  // included — so an ungated check reports a split home for a plain `svc`, which is not a claim
  // about x18 at all. (It did, and it made a veneered control arm look like the test arm.)
  int uses_px18 = (imm == GMEM_BRK_X18_ENTER || imm == GMEM_BRK_X18_LEAVE)
               || (imm == BRK_CTR_READ + 18) || (imm == BRK_TLS_READ + 18)
               || (imm == BRK_TLS_WRITE + 18)
               || (imm == BRK_DC_ZVA + 18) || (imm == BRK_MRS_DCZID + 18);
  if (uses_px18 && !(gt && gt->x18_slot) && gmem_x18_tsd_on()) {
    static unsigned long long n_split;
    if (++n_split == 1 || (n_split % 10000) == 0)
      fprintf(stderr, "[x18home] SPLIT HOME: guest thread with no bound x18 slot is using the "
                      "shared global on the trap path while its veneers use per-thread TSD "
                      "(%llu trap(s) so far, gt=%p)\n", n_split, (void*)gt);
  }
  static unsigned long long g_saved0;   // pre-threading fallback, same shape as gmem_guest_x18
  unsigned long long* psaved = gt ? &gt->x18_saved : &g_saved0;

  if (imm == GMEM_BRK_X18_ENTER || imm == GMEM_BRK_X18_LEAVE) {  // x18 site on the trap path
    unsigned long next = 0;
    int ok = (imm == GMEM_BRK_X18_ENTER)
        ? gmem_x18_trap_enter((unsigned long)ts->__pc, ns->__x, px18, psaved, &next)
        : gmem_x18_trap_leave((unsigned long)ts->__pc, ns->__x, px18, psaved, &next);
    if (imm == GMEM_BRK_X18_ENTER) TRAPCNT(g_n_x18_enter); else TRAPCNT(g_n_x18_leave);
    if (ok) { ns->__pc = next; return; }
    fprintf(stderr, "[shim] unclaimed x18 trap at %#llx\n", (unsigned long long)ts->__pc);
    _exit(43);
  }
  // NOTE on x18 in all three of these: x18 is itself virtualized, so when it is the destination
  // (or source) the value belongs in the SHADOW, not in the hardware register the guest can never
  // keep. Writing ns->__x[18] would be silently discarded by macOS.
  if (imm >= BRK_CTR_READ && imm < BRK_CTR_READ + 32) {          // mrs xN, ctr_el0
    TRAPCNT(g_n_ctr);
    unsigned rt = imm - BRK_CTR_READ;
    unsigned long v = ctr_el0_value();
    if (rt == 18)      *px18 = v;
    else if (rt != 31) ns->__x[rt] = v;                          // 31 is xzr: discard
  } else if (imm >= BRK_TLS_READ && imm < BRK_TLS_READ + 32) {   // mrs xN, tpidr_el0
    TRAPCNT(g_n_tls); TRAPCNT(g_n_tlsr);
    unsigned rt = imm - BRK_TLS_READ;
    if (rt == 18)      *px18 = *tpidr;
    else if (rt != 31) ns->__x[rt] = *tpidr;
  } else if (imm >= BRK_TLS_WRITE && imm < BRK_TLS_WRITE + 32) { // msr tpidr_el0, xN
    TRAPCNT(g_n_tls); TRAPCNT(g_n_tlsw);
    unsigned rt = imm - BRK_TLS_WRITE;
    *tpidr = (rt == 18) ? *px18 : (rt == 31 ? 0 : ts->__x[rt]);
    if (g_verbose) { fprintf(stderr, "[tpidr] tid=%d set=%#lx\n", guest_tid(), *tpidr); fflush(stderr); }
  } else if (imm >= BRK_DC_ZVA && imm < BRK_DC_ZVA + 32) {       // dc zva, xN
    TRAPCNT(g_n_dczva);
    unsigned rt = imm - BRK_DC_ZVA;
    unsigned long addr = (rt == 18) ? *px18 : (rt == 31 ? 0 : ts->__x[rt]);
    if (addr) memset((void*)addr, 0, 64);                        // zero one 64-byte line
  } else if (imm >= BRK_MRS_DCZID && imm < BRK_MRS_DCZID + 32) { // mrs xN, DCZID_EL0
    TRAPCNT(g_n_dczid);
    unsigned rt = imm - BRK_MRS_DCZID;
    if (rt == 18)      *px18 = 0x4;                              // log2(64/4)=4 -> 64-byte granule
    else if (rt != 31) ns->__x[rt] = 0x4;
  } else {                                                       // brk #0 == Linux syscall
    TRAPCNT(g_n_sc);
    unsigned long nr = ts->__x[8];
    if (nr == 220) {                                             // clone: needs the register file
      ns->__x[0] = (unsigned long)do_clone(ts);
      ns->__pc = ts->__pc + 4;
      return;
    }
    // A guest that aborts leaves NO message (measured: the WebKitWebProcess writes nothing to fd 1
    // or 2 before its tgkill(SIGABRT)), so the only way to learn WHO called abort() is the guest's
    // own call chain -- available right here, because the trap handler holds the full register
    // file. Walk the AAPCS frame-pointer chain: x29 -> [saved x29, saved x30].
    if (g_sctrace_fx && (nr == 129 || nr == 130 || nr == 131)) {
      fx_note("ABORTPATH nr=%lu pc=%#llx lr=%#llx fp=%#llx sp=%#llx\n",
              nr, ts->__pc, ts->__lr, ts->__fp, ts->__sp);
      unsigned long fp = (unsigned long)ts->__fp;
      for (int i = 0; i < 24 && fp; i++) {
        unsigned long* f = (unsigned long*)fp;
        unsigned long nextfp = f[0], lr = f[1];
        fx_note("ABORTPATH   #%d lr=%#lx\n", i, lr);
        if (nextfp <= fp || nextfp - fp > 0x100000) break;
        fp = nextfp;
      }
    }
    long r = (g_census == 1 ? route_syscall_census : route_syscall)(
                               nr, ts->__x[0], ts->__x[1], ts->__x[2],
                               ts->__x[3], ts->__x[4], ts->__x[5]);
    if (g_verbose) {
      fprintf(stderr, "[sc] nr=%lu(%#llx,%#llx,%#llx) -> %ld (%#lx)\n", nr,
              ts->__x[0], ts->__x[1], ts->__x[2], r, (unsigned long)r);
      fflush(stderr);
    }
    // RDFUTEX_SC=1: the same trace, but into the PER-PROCESS futex log. Three shim processes
    // share one stderr and the [sc] lines carry no pid, so a shared trace cannot say which
    // process made the call that preceded the abort -- which is the only question being asked.
    if (g_sctrace_fx)
      fx_note("SC nr=%lu(%#llx,%#llx,%#llx,%#llx,%#llx,%#llx) -> %ld (%#lx)\n", nr,
              ts->__x[0], ts->__x[1], ts->__x[2], ts->__x[3], ts->__x[4], ts->__x[5],
              r, (unsigned long)r);
    ns->__x[0] = (unsigned long)r;
  }
  // Resume past the brk. No trampoline is needed: the guest never reads the hardware TPIDR_EL0.
  ns->__pc = ts->__pc + 4;
}
// --- in-thread brk servicing (RDSIGTRAP=1) ---------------------------------------------------
// Three things make this different from the Mach path, and all three are load-bearing:
//
//  1. IDENTITY. There is no message to carry a thread port, but we ARE the faulting thread, so
//     the gthread record can simply be cached in thread-local storage after the first trap.
//     mach_thread_self() takes a reference every call, so it is fetched once per thread.
//
//  2. JIT WRITE PROTECTION. Guest text is MAP_JIT, and every guest thread sets it EXECUTABLE
//     (pthread_jit_write_protect_np(1)) before entering the guest. The Mach handler got away
//     with writing guest code -- gmem_map_code() when dlopen brings in a new image, the x18
//     rewriter, fastcall patching -- because it ran on the exception-server thread, where the
//     mapping is still WRITABLE. In-thread it is not, so the handler must flip and flip back.
//     That is an APRR register write, not a syscall.
//
//  3. SIGNAL MASK. A synchronous SIGTRAP that arrives BLOCKED is fatal, and pthread_create
//     inherits the creating thread's mask -- so every thread spawned from inside this handler
//     (do_clone, do_fork, do_vfork all run here) starts with SIGTRAP blocked and would die on
//     its first guest instruction. sigtrap_thread_setup() unblocks it on the new thread.
//
// The guest's SP is irrelevant here even though pixman deliberately runs misaligned: SA_ONSTACK
// puts the signal frame on our own alt stack, and XNU aligns the frame pointer regardless.
static _Thread_local mach_port_t   t_selfport;
static _Thread_local struct gthread* t_gt_cached;

static void sigtrap_handler(int sig, siginfo_t* si, void* uc_) {
  (void)sig; (void)si;
  ucontext_t* uc = (ucontext_t*)uc_;
  arm_thread_state64_t* st = (arm_thread_state64_t*)&uc->uc_mcontext->__ss;
  if (!t_gt_cached) {
    if (!t_selfport) t_selfport = mach_thread_self();
    t_gt_cached = gt_for(t_selfport);
  }
  pthread_jit_write_protect_np(0);
  service_brk(st, st, t_gt_cached);          // ts and ns alias: see service_brk's contract
  pthread_jit_write_protect_np(1);
}

static void sigtrap_thread_setup(void) {
  if (!g_sigtrap_mode) return;
  size_t sz = (size_t)SIGSTKSZ * 8;
  void* p = malloc(sz);
  if (p) { stack_t ss = { .ss_sp = p, .ss_size = sz, .ss_flags = 0 }; sigaltstack(&ss, NULL); }
  // RDSIGTRAP_NOUNBLOCK=1 skips this on purpose, to demonstrate that it is load-bearing rather
  // than defensive: without it, any thread created from inside the handler (every clone()) starts
  // with SIGTRAP blocked, and a blocked SYNCHRONOUS signal is forced to the default action --
  // the guest dies on its first rewritten instruction.
  if (!getenv("RDSIGTRAP_NOUNBLOCK")) {
    sigset_t m; sigemptyset(&m); sigaddset(&m, SIGTRAP);
    pthread_sigmask(SIG_UNBLOCK, &m, NULL);
  }
  t_gt_cached = NULL;                        // this thread has its own gthread record
}

static void sigtrap_install(void) {
  struct sigaction sa; memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = sigtrap_handler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTRAP, &sa, NULL);
}

// EXC_MASK_BREAKPOINT is what routes a `brk` to the Mach port. Drop it and the kernel falls back
// to the BSD signal. BAD_ACCESS/BAD_INSTRUCTION stay on Mach: they are fatal diagnostics, not a
// hot path, and the existing handler already prints them.
static exception_mask_t exc_mask(void) {
  exception_mask_t m = EXC_MASK_BAD_ACCESS | EXC_MASK_BAD_INSTRUCTION;
  if (!g_sigtrap_mode) m |= EXC_MASK_BREAKPOINT;
  return m;
}

kern_return_t catch_mach_exception_raise(mach_port_t a, mach_port_t b, mach_port_t c,
  exception_type_t d, mach_exception_data_t e, mach_msg_type_number_t f) {
  (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; return KERN_FAILURE; }
kern_return_t catch_mach_exception_raise_state(mach_port_t a, exception_type_t b,
  mach_exception_data_t c, mach_msg_type_number_t d, int* e, thread_state_t f,
  mach_msg_type_number_t g, thread_state_t h, mach_msg_type_number_t* i) {
  (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i; return KERN_FAILURE; }

static dispatch_semaphore_t g_exc_ready;
static void* exc_loop(void* _) {
  (void)_;
  dispatch_semaphore_signal(g_exc_ready);   // the port already holds the receive right
  while (1) mach_msg_server(mach_exc_server, 4096, g_port, 0);
  return 0;
}

struct guestctx { struct prog_info pi; void* sp; };
static void* guest_thread(void* arg) {
  struct guestctx* g = (struct guestctx*)arg;

  // Register the MAIN guest thread here rather than letting the trap handler create its record
  // lazily on the first trap. The record itself the handler could still invent — but x18_slot it
  // could not, because binding a slot has to happen on the owning thread and the handler is not
  // it. Without this the main thread would be the one thread whose veneers and traps disagreed.
  // Seed 0: x18 is undefined at process entry, and this is a fresh pthread whose TSD is zero
  // anyway, so this only publishes the address.
  {
    struct gthread* self = gt_for(mach_thread_self());
    if (self) { self->tid = 1; self->is_child = 0; }
    gt_bind_x18(self, 0);
  }

  pthread_jit_write_protect_np(1);          // JIT text executable on this thread
  sigtrap_thread_setup();
  fastcall_thread_init();                   // per-thread native stack for the syscall fast path
  fprintf(stderr, "[shim] entering guest on worker thread...\n\n");
  enter_guest(entry_point(&g->pi), g->sp);  // never returns
  return 0;
}

// --- guest environment -----------------------------------------------------------------------
// The guest gets a fixed base environment, but every experiment we run (indirect GLX vs Mesa's
// own software rasterizer, which driver, how much logging) is selected by an env var, so the
// host must be able to override it. Two mechanisms, both host-side:
//
//   LIBGL_*/MESA_*/GALLIUM_*/EGL_*   forwarded verbatim if set on the host
//   RDENV_FOO=bar                    passes FOO=bar (escape hatch for anything else)
//
// A forwarded name REPLACES the base entry of the same name, and an EMPTY value REMOVES it
// entirely rather than passing an empty string. That distinction is not pedantry: Mesa tests
// `getenv("LIBGL_ALWAYS_INDIRECT") != NULL`, so setting it to "0" still selects indirect GLX.
// `LIBGL_ALWAYS_INDIRECT= ./shim_gui ...` is what actually turns the default off.
extern char** environ;

static const char* kEnvPrefixes[] = { "LIBGL_", "MESA_", "GALLIUM_", "EGL_", "GBM_", 0 };

static int env_forwarded(const char* s) {
  for (int i = 0; kEnvPrefixes[i]; i++)
    if (!strncmp(s, kEnvPrefixes[i], strlen(kEnvPrefixes[i]))) return 1;
  return 0;
}

// Name part of "NAME=VALUE", including the '='; used so an override displaces the base entry.
static size_t env_namelen(const char* s) {
  const char* eq = strchr(s, '=');
  return eq ? (size_t)(eq - s + 1) : strlen(s);
}

static char** build_guest_env(void) {
  static char* base[] = {
    "PATH=/bin:/usr/bin", "TERM=xterm", "HOME=/root", "DISPLAY=:0",
    "XDG_RUNTIME_DIR=/tmp",
    // WAYLAND_DISPLAY, for the same reason DISPLAY is here. GDK does not need it --
    // it defaults the socket name to "wayland-0" -- so every GTK app worked without
    // it and its absence was invisible. But an app is entitled to READ it, and one
    // that does treats "unset" as "there is no Wayland here".
    //
    // MEASURED: Firefox 140's IsWaylandEnabled() reads WAYLAND_DISPLAY directly and
    // FORCES GDK_BACKEND=x11 when it is unset -- overriding GDK_BACKEND=wayland that
    // the host explicitly passed. It then dies with "Error: cannot open display: :0",
    // which names the X11 display and so reads as an X11 problem, while gtk3-demo in
    // the SAME rootfs presents 1490 frames over Wayland. Two runs lost to it.
    "WAYLAND_DISPLAY=wayland-0",
    "LIBGL_ALWAYS_INDIRECT=1",   // default: GL over the wire -> our GLX decoder -> Metal
  };
  const int nbase = (int)(sizeof base / sizeof base[0]);

  // Handed a guest environment by an exec'ing shim: pass it through untouched.
  if (getenv("RDGUESTENV")) {
    int n = 0; for (char** e = environ; *e; e++) n++;
    char** out = calloc((size_t)n + 1, sizeof *out);
    if (out) {
      int k = 0;
      for (char** e = environ; *e; e++) {
        if (!strncmp(*e, "RDGUESTENV=", 11)) continue;
        if (!strncmp(*e, "RD", 2) || !strncmp(*e, "X11SRV_", 7) || !strncmp(*e, "WLSRV_", 6))
          continue;                            // shim controls, not the guest's environment
        out[k++] = *e;
      }
      out[k] = 0;
      return out;
    }
  }

  int nhost = 0; for (char** e = environ; *e; e++) nhost++;
  char** out = calloc((size_t)nbase + nhost + 1, sizeof *out);
  if (!out) return base;                      // OOM: the base env is still valid
  int n = 0;

  for (char** e = environ; *e; e++) {
    const char* s = *e;
    if (env_forwarded(s)) { out[n++] = (char*)s; continue; }
    if (!strncmp(s, "RDENV_", 6)) {
      const char* rest = s + 6;
      if (strchr(rest, '=')) out[n++] = (char*)rest;
    }
  }
  // Base entries come last, and only if nothing forwarded already defines that name.
  for (int i = 0; i < nbase; i++) {
    size_t nl = env_namelen(base[i]);
    int shadowed = 0;
    for (int j = 0; j < n && !shadowed; j++)
      if (!strncmp(out[j], base[i], nl)) shadowed = 1;
    if (!shadowed) out[n++] = base[i];
  }
  out[n] = 0;

  // Drop the empty-valued entries now that they have done their shadowing job: they were only
  // ever a way for the host to say "do not pass this variable at all".
  int k = 0;
  for (int i = 0; i < n; i++) {
    const char* eq = strchr(out[i], '=');
    if (eq && eq[1] == '\0') continue;
    out[k++] = out[i];
  }
  out[k] = 0;

  if (getenv("RDVERBOSE"))
    for (int i = 0; i < k; i++) fprintf(stderr, "[shim] env %s\n", out[i]);
  return out;
}

// Reap /tmp/rdx-<pid> namespaces whose owning process is gone.
//
// 62c2f30 gave every instance its own AF_UNIX namespace to stop concurrent shims stealing one
// another's compositor. It did not give them a reaper, and nothing else has one: a four-hour
// session left 623 stale directories behind. atexit() alone would not fix that, because the
// interesting exits here are crashes and SIGKILLs -- exactly the cases that skip it. So sweep at
// STARTUP instead, which is self-healing after any kind of death, including a kill -9.
//
// kill(pid, 0) is the liveness test, and the failure modes matter: ESRCH means the pid is gone and
// the directory is ours to remove; EPERM means it exists but belongs to another user, so leave it
// alone. Anything that does not parse as our exact "/tmp/rdx-<digits>" shape is skipped untouched,
// so a hand-made RDUNIXPREFIX directory is never a candidate. Best-effort throughout: a namespace
// we fail to remove costs a directory, while removing a live one would steal a running guest's
// compositor -- which is the very bug 62c2f30 fixed, so the asymmetry decides every judgement call.
static void reap_stale_unix_prefixes(void) {
  DIR* d = opendir("/tmp");
  if (!d) return;
  struct dirent* e;
  int reaped = 0;
  while ((e = readdir(d))) {
    if (strncmp(e->d_name, "rdx-", 4) != 0) continue;
    const char* p = e->d_name + 4;
    if (!*p) continue;
    for (const char* q = p; *q; q++) if (*q < '0' || *q > '9') { p = NULL; break; }
    if (!p) continue;                       // "rdx-something-else": not ours to judge
    long pid = strtol(p, NULL, 10);
    if (pid <= 0 || pid == (long)getpid()) continue;
    if (kill((pid_t)pid, 0) == 0) continue;         // alive: leave it strictly alone
    if (errno == EPERM) continue;                   // exists, another user: not ours
    char path[128], cmd[256];
    snprintf(path, sizeof path, "/tmp/rdx-%ld", pid);
    // The tree holds socket nodes and nested dirs; rmdir() will not do. execl'ing rm would be
    // cleaner than system(), but this runs before any guest thread exists and the paths are
    // built from digits we just validated, so there is nothing here for a shell to misread.
    snprintf(cmd, sizeof cmd, "/bin/rm -rf '%s'", path);
    if (system(cmd) == 0) reaped++;
  }
  closedir(d);
  if (reaped && getenv("RDVERBOSE"))
    fprintf(stderr, "[shim] reaped %d stale /tmp/rdx-<pid> namespace(s)\n", reaped);
}

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s <rootfs> <guest-path> [args...]\n", argv[0]); return 2; }
  g_verbose = getenv("RDVERBOSE") != 0;
  g_sctrace_fx = getenv("RDFUTEX_SC") != 0;

  // Take a PRIVATE copy of stderr before the guest can touch it. The guest owns fd 0/1/2 and
  // reassigns them freely — Qt does `dup3(12, 2, 0)` during startup, pointing fd 2 at a pipe of
  // its own. Every diagnostic we then write lands in that pipe, nobody drains it, it fills at
  // 64 KB, and our write() blocks forever inside the exception handler — which freezes the whole
  // guest. It looks exactly like a hang in the app.
  //
  // macOS's stderr is a writable global (__stderrp), so repointing the C stream at our own dup
  // keeps every fprintf(stderr, ...) in the shim going somewhere the guest cannot take away.
  {
    // Placed HIGH and registered as shim-owned. CLOEXEC alone does not protect it: the guest
    // shares our fd table, and close_range(3, ~0U, 0) -- the ordinary "close everything before
    // exec" idiom -- used to close this descriptor and every shim diagnostic after it vanished
    // with nothing said. MEASURED before the fix: with RDPATHS=1 the [path] line for an openat
    // BEFORE close_range appeared and the one AFTER did not. See tests/wl/closerange.c.
    int priv = fcntl(2, F_DUPFD_CLOEXEC, 900);
    if (priv < 0) priv = dup(2);                  // no high slot free: still better than nothing
    if (priv >= 0) {
      fcntl(priv, F_SETFD, FD_CLOEXEC);
      shim_fd_reserve(priv);
      FILE* f = fdopen(priv, "w");
      if (f) { setvbuf(f, NULL, _IOLBF, 0); stderr = f; }
      else { shim_fd_unreserve(priv); close(priv); }
    }
  }
  const char* rootfs = argv[1];
  snprintf(g_self_path, sizeof g_self_path, "%s", argv[0]);
  snprintf(g_rootfs_path, sizeof g_rootfs_path, "%s", rootfs);
  const char* guestpath = argv[2];          // guest-absolute, e.g. /bin/busybox
  idle_trace_fd13("new-shim-entry", guestpath);

  gmem_init();
  gmem_x18_tsd_init();      // pick x18's home before any thread can bind its address
  if (!getenv("RDNOVDSO")) vdso_build();   // A/B: RDNOVDSO=1 forces libc onto real syscalls
  gpu_init();
  syscalls_set_root(rootfs);
  syscalls_ext_set_root(rootfs);

  // ---- #! ------------------------------------------------------------------------------------
  // On Linux the KERNEL implements the shebang: execve() on a file whose first two bytes are "#!"
  // runs the named interpreter with the script path as its first argument. The shim had no
  // equivalent at all, so exec'ing ANY script reached the ELF loader and died with
  //   [elfload] ...: not an ELF (bad magic 23 21 2f 62)      <- that is "#!/b"
  // Because guest execve is serviced by re-exec'ing the shim, doing it HERE covers both the
  // top-level `./shim_cli <rootfs> /some/script` and every execve the guest performs.
  //
  // This is not a corner case: dpkg runs every maintainer script by exec'ing
  // /var/lib/dpkg/info/<pkg>.postinst, which is `#!/bin/sh`. 62 packages in this rootfs ship one.
  // Without it dpkg can unpack but can never configure anything that has a script, apt-key (a
  // shell script) cannot run, and `find -exec ./script` returns ENOEXEC.
  //
  // Linux semantics, followed exactly: the interpreter is the first whitespace-delimited token;
  // EVERYTHING after it on the line is ONE argument (not word-split); the script path is passed as
  // given; nesting is capped (Linux: BINPRM_MAX_RECURSION = 4).
  {
    char sbhost[2048];
    for (int depth = 0; depth < 4; depth++) {
      if (guest_host_path(rootfs, argv[2], sbhost, sizeof sbhost) != 0) break;
      int sfd = open(sbhost, O_RDONLY);
      if (sfd < 0) break;
      char line[257];
      ssize_t n = read(sfd, line, sizeof line - 1);
      close(sfd);
      if (n < 3 || line[0] != '#' || line[1] != '!') break;
      line[n] = 0;
      char* nl = strchr(line, '\n'); if (nl) *nl = 0;
      char* p = line + 2;
      while (*p == ' ' || *p == '\t') p++;
      if (!*p) break;                                  // "#!" with nothing after it
      char* arg = 0;
      char* q = p;
      while (*q && *q != ' ' && *q != '\t') q++;
      if (*q) {
        *q++ = 0;
        while (*q == ' ' || *q == '\t') q++;
        size_t L = strlen(q);
        while (L && (q[L-1] == ' ' || q[L-1] == '\t')) q[--L] = 0;
        if (L) arg = q;
      }
      char** na = (char**)calloc((size_t)argc + 3, sizeof *na);
      if (!na) break;
      int k = 0;
      na[k++] = argv[0];                               // shim
      na[k++] = argv[1];                               // rootfs
      na[k++] = strdup(p);                             // interpreter, guest-absolute
      if (arg) na[k++] = strdup(arg);                  // its single optional argument
      for (int i = 2; i < argc; i++) na[k++] = argv[i];  // script path, then the script's own args
      na[k] = 0;
      argv = na; argc = k;
      if (g_verbose) fprintf(stderr, "[shim] #! %s%s%s -> %s\n", p, arg ? " " : "",
                             arg ? arg : "", argv[2 + (arg ? 1 : 0)]);
    }
    guestpath = argv[2];
  }

  syscalls_set_exe(guestpath);
  // Restore the cwd an exec'ing parent shim handed us (see guest_execve). Must come after
  // syscalls_set_root, which does not touch g_cwd but owns everything else about resolution.
  { const char* cw = getenv("RDGUESTCWD");
    if (cw && cw[0] == '/') syscalls_set_cwd(cw);
    else {
      // START WHERE THE USER IS. Without this the guest always begins at "/", so running a binary
      // from your own project directory lands in the rootfs root and every relative path is wrong.
      // If the shim's cwd is inside a shared host prefix (/Users, /Volumes -- see host_share() in
      // syscalls.c) that path means the SAME thing on both sides and can be handed straight over.
      // Outside a shared prefix there is no meaningful guest equivalent and "/" stays correct.
      char hcwd[1024];
      if (getcwd(hcwd, sizeof hcwd) &&
          (!strncmp(hcwd, "/Users/", 7) || !strncmp(hcwd, "/Volumes/", 9)))
        syscalls_set_cwd(hcwd);
    } }
  net_init();
  // PER-PROCESS AF_UNIX NAMESPACE. This used to be the fixed "/tmp/rdx" for every instance, so two
  // concurrent shim_gui processes bound the SAME /tmp/rdx/tmp/wayland-0: the first to bind owned the
  // compositor and every later one silently became a CLIENT of it. The result is that one instance's
  // window renders another instance's guest -- confirmed by lsof showing two unrelated shim_gui
  // processes on that single socket, and by a run launched on example.com displaying a peer's
  // webkitgtk.org page under its own CGWindowOwnerPID. Worse, wlsrv_start() unlinks unconditionally
  // before bind(), so a later starter steals the name and earlier guests are left connected to
  // nothing, and any instance exiting tears the shared compositor down under all the others -- which
  // is why runs "died" in batches with clean logs and no crash report.
  //
  // This is the SAME bug, with the same reasoning, that the WL_HEADLESS block below already fixed
  // for shim_wlh (see its comment: "R4 caught three shim_wlh processes on that single path at
  // once"). That lesson simply never got applied six lines up, to the path that shim_gui actually
  // uses. Children have always been per-pid (do_fork, "/tmp/rdx-%d"), so the convention is proven.
  //
  // Nothing external needs a well-known name: the guest reaches the socket through the
  // XDG_RUNTIME_DIR/WAYLAND_DISPLAY that the shim itself sets. RDUNIXPREFIX still overrides, for
  // deliberately pointing two shims at one namespace.
  char autopfx[64];
  const char* uxpfx = getenv("RDUNIXPREFIX");
  if (!uxpfx || !*uxpfx) {
    snprintf(autopfx, sizeof autopfx, "/tmp/rdx-%d", (int)getpid());
    uxpfx = autopfx;
    reap_stale_unix_prefixes();   // see below: per-pid namespaces have no other reaper
  }
  net_set_unix_prefix(uxpfx);
  g_census = (getenv("RDCENSUS") != NULL);            // P2 instrument (measurement only)
  fastcall_set_dispatch(g_census == 1 ? route_syscall_census : route_syscall_fc);
#ifndef NO_GPU
  {
    // mkdir(2), not system("mkdir -p"): a fork()ed child inherits Apple libc FILE locks (glibc
    // resets them), so shelling out here has bitten this file before.
    char dir[1100], sock[1200];
    snprintf(dir, sizeof dir, "%s", uxpfx);                 mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/tmp", uxpfx);             mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/tmp/.X11-unix", uxpfx);   mkdir(dir, 0700);
    snprintf(sock, sizeof sock, "%s/tmp/.X11-unix/X0", uxpfx);
    x11srv_start(sock);
    // The Wayland compositor listens alongside the X server; the guest picks by connecting to one
    // or the other. $XDG_RUNTIME_DIR is /tmp in the guest, so this is the guest's /tmp/wayland-0.
    snprintf(sock, sizeof sock, "%s/tmp/wayland-0", uxpfx);
    wlsrv_start(sock);
  }
  g_display_up = 1;                  // fork() must rebuild these in the child; see do_fork
#endif
#ifdef WL_HEADLESS
  // Diagnostic build: the Wayland compositor without Metal/Cocoa. Presentation and host input
  // are stubbed in wlhead_stub.c, so the whole protocol path can be traced without a window.
  // RDUNIXPREFIX moves the whole AF_UNIX namespace aside so this instance cannot unlink() the
  // /tmp/rdx/tmp/wayland-0 socket belonging to a concurrently running shim_gui.
  {
    // DEFAULT TO A PER-PROCESS NAMESPACE. This used to default to "/tmp/rdx" -- the SAME prefix
    // shim_gui uses -- so every headless instance bound, and unlink()ed, the one shared
    // /tmp/rdx/tmp/wayland-0. R4 caught three shim_wlh processes on that single path at once,
    // with three different socket inodes: each wlsrv_start() unlinks unconditionally before
    // bind(), so the last starter silently steals the name and the earlier guests are left
    // connected to nothing. The signature is a process at ~0.00s CPU with a listen socket and no
    // client, which reads exactly like "the app is running but rendering nothing" -- it cost R5's
    // kate runs and contaminated R3's timings.
    //
    // The guest reaches the socket through XDG_RUNTIME_DIR/WAYLAND_DISPLAY that the shim itself
    // sets, so there is no external consumer needing a well-known name and nothing to lose by
    // isolating. RDUNIXPREFIX still overrides, for the rare case of pointing two shims at one
    // namespace deliberately.
    char autopfx[64];
    const char* pfx = getenv("RDUNIXPREFIX");
    if (!pfx || !*pfx) {
      snprintf(autopfx, sizeof autopfx, "/tmp/rdx-%d", (int)getpid());
      pfx = autopfx;
    }
    net_set_unix_prefix(pfx);
    char cmd[1200], sock[1100];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s/tmp'", pfx);
    if (system(cmd) != 0) {}
    snprintf(sock, sizeof sock, "%s/tmp/wayland-0", pfx);
    wlsrv_start(sock);
    g_display_up = 1;                // fork() must rebuild this in the child; see do_fork
  }
#endif


  char host[1024]; guest_host_path(rootfs, guestpath, host, sizeof host);
  struct loaded prog = {0};
  if (elf_load(host, &prog) != 0) { fprintf(stderr, "[shim] failed to load %s\n", host); return 1; }
  fprintf(stderr, "[shim] loaded %s base=%#lx entry=%#lx interp='%s'\n",
          guestpath, prog.base, prog.entry, prog.interp);

  g_main_base = prog.base;
  struct prog_info pi = {0};
  pi.base = prog.base; pi.entry = prog.entry; pi.phdr = prog.phdr;
  pi.phnum = prog.phnum; pi.phent = prog.phent;
  if (prog.interp[0]) {
    char ihost[1024]; guest_host_path(rootfs, prog.interp, ihost, sizeof ihost);
    struct loaded interp = {0};
    if (elf_load(ihost, &interp) != 0) { fprintf(stderr, "[shim] failed interp %s\n", ihost); return 1; }
    pi.interp_base = interp.base; pi.interp_entry = interp.entry; g_interp_base = interp.base;
    fprintf(stderr, "[shim] loaded interp %s base=%#lx entry=%#lx\n",
            prog.interp, interp.base, interp.entry);
  }

  size_t stksz = 8 * 1024 * 1024;
  void* stack = malloc(stksz);              // plain RW is fine for the guest stack
  int gargc = argc - 2; char** gargv = &argv[2];
  char** envp = build_guest_env();
  void* sp = build_stack(stack, stksz, gargc, gargv, envp, &pi);
  // Tell /proc/self/maps where the main guest stack REALLY is. glibc's pthread_getattr_np()
  // recovers the initial thread's stack by finding the maps line containing __libc_stack_end,
  // and WTF::StackBounds is built on it -- see the comment on proc_maps() in syscalls.c.
  syscalls_set_main_stack((unsigned long)(uintptr_t)stack,
                          (unsigned long)(uintptr_t)stack + stksz);

  { const char* v = getenv("RDSIGTRAP"); g_sigtrap_mode = !v || (*v && strcmp(v, "0")); }
  if (g_sigtrap_mode) { sigtrap_install(); fprintf(stderr, "[shim] brk serviced IN-THREAD (SIGTRAP)\n"); }
  { const char* v = getenv("RDTRAPSTAT");
    if (v && *v && strcmp(v, "0")) {
      pthread_t st; if (pthread_create(&st, NULL, trapstat_loop, NULL) == 0) pthread_detach(st);
    } }
  { const char* v = getenv("RDTHREADDUMP_SECS");
    if (v && *v && strcmp(v, "0")) {
      pthread_t st; if (pthread_create(&st, NULL, threaddump_loop, NULL) == 0) pthread_detach(st);
    } }

  mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &g_port);
  mach_port_insert_right(mach_task_self(), g_port, g_port, MACH_MSG_TYPE_MAKE_SEND);
  // EXC_BAD_INSTRUCTION matters as much as the other two: without it a guest SIGILL never reaches
  // us at all. That is exactly how the `mrs ctr_el0` gap hid — llvmpipe looked like a silent hang
  // for hours because the fault went to the default handler and produced nothing in our log.
  task_set_exception_ports(mach_task_self(), exc_mask(), g_port,
      EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES, ARM_THREAD_STATE64);
  g_exc_ready = dispatch_semaphore_create(0);
  pthread_t ext; pthread_create(&ext, NULL, exc_loop, NULL);
  dispatch_semaphore_wait(g_exc_ready, DISPATCH_TIME_FOREVER);  // was a blind 20ms sleep

  static struct guestctx g; g.pi = pi; g.sp = sp;
  pthread_t gt; pthread_create(&gt, NULL, guest_thread, &g);

  gpu_run_event_loop();                     // main thread: [NSApp run]
  return 0;
}
