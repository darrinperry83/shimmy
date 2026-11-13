// test_fastcall.c — standalone proof of the fastcall fast path: SPEED and SAFETY.
//
// gmem.c is being written in parallel, so this file provides local MAP_JIT-backed stubs for the
// four gmem entry points fastcall.c uses. Build:
//
//   clang -arch arm64 -O2 -Wall -o test_fastcall test_fastcall.c fastcall.c
//   codesign --force --sign - --entitlements ent.plist test_fastcall     # MAP_JIT needs allow-jit
//   ./test_fastcall
//
// Tests
//   A  patched site:   guest-like non-leaf wrapper (stp x29,x30 / svc / ldp x29,x30 / ret)
//                      -> svc rewritten to brk -> patched to `bl thunk` -> called for real.
//   A2 the literal `mov x8,#nr ; svc #0 ; ret` shape: x30 is LIVE at the site (the `ret` reads it)
//                      so the patcher must SKIP it. Kept as a second skip witness.
//   B  x30 REGRESSION: the musl leaf idiom `mov x8,#nr ; svc #0 ; b __syscall_ret` must be
//                      SKIPPED, bytes unchanged, and must still execute and RETURN.
//   C  x18 and v0-v31 preserved across a fast syscall; x18 seen as ZERO by the host dispatch.
//   D  1,000,000 fast calls -> ns/call.

#include "fastcall.h"
#include "gmem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <pthread.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach_time.h>

// =============================================================================================
// local gmem stubs (MAP_JIT + per-thread write-protect toggle)
// =============================================================================================

#define MAX_REGIONS 32
static struct { unsigned char* base; size_t len; } g_regions[MAX_REGIONS];
static int g_nregions;
static int g_load_depth;

void gmem_init(void) {}

void* gmem_reserve(size_t len)
{
    len = (len + 16383u) & ~(size_t)16383u;
    void* p = mmap(NULL, len, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (p == MAP_FAILED) { perror("mmap MAP_JIT"); return NULL; }
    if (g_nregions < MAX_REGIONS) {
        g_regions[g_nregions].base = (unsigned char*)p;
        g_regions[g_nregions].len  = len;
        g_nregions++;
    }
    return p;
}

void gmem_begin_load(void) { if (g_load_depth++ == 0) pthread_jit_write_protect_np(0); }
void gmem_end_load(void)   { if (--g_load_depth == 0) pthread_jit_write_protect_np(1); }

int gmem_map_code(void* addr, const void* bytes, size_t len)
{
    int own = (g_load_depth == 0);
    if (own) pthread_jit_write_protect_np(0);
    memcpy(addr, bytes, len);
    if (own) pthread_jit_write_protect_np(1);
    sys_icache_invalidate(addr, len);
    return 0;
}

// Unused by fastcall.c, but declared in gmem.h — provide trivial definitions so nothing surprises.
long gmem_mmap(unsigned long a, size_t l, int p, int f, int fd, long o)
{ (void)a;(void)l;(void)p;(void)f;(void)fd;(void)o; return -38; }
long gmem_mprotect(unsigned long a, size_t l, int p) { (void)a;(void)l;(void)p; return -38; }
long gmem_munmap(unsigned long a, size_t l) { (void)a;(void)l; return -38; }

// The real one lives in gmem.c; this mirrors the documented rewrite so the test can drive the
// same pipeline (svc -> brk) that fastcall_patch_ex consumes.
void gmem_rewrite_exec_insns(unsigned char* buf, size_t len)
{
    uint32_t* w = (uint32_t*)buf;
    for (size_t i = 0; i + 4 <= len; i += 4, w++) {
        if (*w == 0xD4000001u) { *w = 0xD4200000u; continue; }              // svc #0 -> brk #0
        if ((*w & 0xFFFFFFE0u) == 0xD51BD040u) { *w = 0xD4200000u | (((0x100u + (*w & 31u)) & 0xFFFFu) << 5); continue; }
        if ((*w & 0xFFFFFFE0u) == 0xD53BD040u) { *w = 0xD4200000u | (((0x200u + (*w & 31u)) & 0xFFFFu) << 5); continue; }
    }
}

// =============================================================================================
// aarch64 encoders (local copy — the test emits its own "guest" code)
// =============================================================================================

#define SP 31u
#define ZR 31u
static uint32_t E_movz(unsigned rd, unsigned imm, unsigned hw) { return 0xD2800000u|(hw<<21)|((imm&0xFFFFu)<<5)|rd; }
static uint32_t E_movk(unsigned rd, unsigned imm, unsigned hw) { return 0xF2800000u|(hw<<21)|((imm&0xFFFFu)<<5)|rd; }
static uint32_t E_svc0(void)  { return 0xD4000001u; }
static uint32_t E_ret(void)   { return 0xD65F03C0u; }
static uint32_t E_stp(unsigned t1,unsigned t2,unsigned rn,int off){ return 0xA9000000u|(((uint32_t)(off/8)&0x7Fu)<<15)|(t2<<10)|(rn<<5)|t1; }
static uint32_t E_ldp(unsigned t1,unsigned t2,unsigned rn,int off){ return 0xA9400000u|(((uint32_t)(off/8)&0x7Fu)<<15)|(t2<<10)|(rn<<5)|t1; }
static uint32_t E_stp_pre(unsigned t1,unsigned t2,unsigned rn,int off){ return 0xA9800000u|(((uint32_t)(off/8)&0x7Fu)<<15)|(t2<<10)|(rn<<5)|t1; }
static uint32_t E_ldp_post(unsigned t1,unsigned t2,unsigned rn,int off){ return 0xA8C00000u|(((uint32_t)(off/8)&0x7Fu)<<15)|(t2<<10)|(rn<<5)|t1; }
static uint32_t E_stp_q(unsigned t1,unsigned t2,unsigned rn,int off){ return 0xAD000000u|(((uint32_t)(off/16)&0x7Fu)<<15)|(t2<<10)|(rn<<5)|t1; }
static uint32_t E_ldp_q(unsigned t1,unsigned t2,unsigned rn,int off){ return 0xAD400000u|(((uint32_t)(off/16)&0x7Fu)<<15)|(t2<<10)|(rn<<5)|t1; }
static uint32_t E_str(unsigned rt,unsigned rn,unsigned off){ return 0xF9000000u|(((off/8)&0xFFFu)<<10)|(rn<<5)|rt; }
static uint32_t E_add_imm(unsigned rd,unsigned rn,unsigned imm){ return 0x91000000u|((imm&0xFFFu)<<10)|(rn<<5)|rd; }
static uint32_t E_subs_imm(unsigned rd,unsigned rn,unsigned imm){ return 0xF1000000u|((imm&0xFFFu)<<10)|(rn<<5)|rd; }
static uint32_t E_mov(unsigned rd,unsigned rm){ return 0xAA0003E0u|(rm<<16)|rd; }
static uint32_t E_dup_2d(unsigned vd,unsigned rn){ return 0x4E080C00u|(rn<<5)|vd; }
static uint32_t E_b(int rel){ return 0x14000000u|((uint32_t)rel & 0x03FFFFFFu); }
static uint32_t E_cbz(unsigned rt,int rel){ return 0xB4000000u|(((uint32_t)rel & 0x7FFFFu)<<5)|rt; }
static uint32_t E_bne(int rel){ return 0x54000001u|(((uint32_t)rel & 0x7FFFFu)<<5); }   // b.ne

// =============================================================================================
// JIT arena
// =============================================================================================

static unsigned char* g_arena;
static size_t g_arena_cap, g_arena_off;

static void* jit_emit(const uint32_t* insns, int n)
{
    size_t need = (size_t)n * 4;
    g_arena_off = (g_arena_off + 15) & ~(size_t)15;
    if (g_arena_off + need > g_arena_cap) { fprintf(stderr, "jit arena full\n"); exit(1); }
    void* dst = g_arena + g_arena_off;
    g_arena_off += need;
    if (gmem_map_code(dst, insns, need) != 0) { fprintf(stderr, "gmem_map_code failed\n"); exit(1); }
    return dst;
}

// =============================================================================================
// the dispatch(es)
// =============================================================================================

#define DISPATCH_RET ((long)0x00ABCDEF12345678LL)

static volatile unsigned long r_nr, r_a[6], r_x18;
static volatile long disp_calls, trap_calls;   // disp_calls counts EVERY dispatch
                                               // invocation; fast = disp_calls - trap_calls.

static unsigned long read_x18(void)
{ unsigned long v; __asm__ volatile("mov %0, x18" : "=r"(v)); return v; }

#define CLB(n) "movi v" #n ".16b, #0x5a\n\t"
static void clobber_all_fp(void)
{
    __asm__ volatile(
        CLB(0)  CLB(1)  CLB(2)  CLB(3)  CLB(4)  CLB(5)  CLB(6)  CLB(7)
        CLB(8)  CLB(9)  CLB(10) CLB(11) CLB(12) CLB(13) CLB(14) CLB(15)
        CLB(16) CLB(17) CLB(18) CLB(19) CLB(20) CLB(21) CLB(22) CLB(23)
        CLB(24) CLB(25) CLB(26) CLB(27) CLB(28) CLB(29) CLB(30) CLB(31)
        ::: "v0","v1","v2","v3","v4","v5","v6","v7",
            "v8","v9","v10","v11","v12","v13","v14","v15",
            "v16","v17","v18","v19","v20","v21","v22","v23",
            "v24","v25","v26","v27","v28","v29","v30","v31");
}

static long recording_dispatch(unsigned long nr, unsigned long a0, unsigned long a1,
                               unsigned long a2, unsigned long a3, unsigned long a4,
                               unsigned long a5)
{
    r_nr = nr; r_a[0]=a0; r_a[1]=a1; r_a[2]=a2; r_a[3]=a3; r_a[4]=a4; r_a[5]=a5;
    r_x18 = read_x18();
    clobber_all_fp();                 // exactly what a real libsystem-using dispatch does
    disp_calls++;
    return DISPATCH_RET;
}

static long bench_dispatch(unsigned long nr, unsigned long a0, unsigned long a1,
                           unsigned long a2, unsigned long a3, unsigned long a4,
                           unsigned long a5)
{ (void)nr;(void)a0;(void)a1;(void)a2;(void)a3;(void)a4;(void)a5; return 0; }

// The trap path, for sites the patcher (correctly) refused to touch. The real shim services
// these on the Mach exception-server thread (~9.7us); an in-thread SIGTRAP is the cheap
// stand-in that lets this standalone test actually EXECUTE a skipped site.
static fastcall_dispatch_fn g_active_dispatch;

static void on_sigtrap(int sig, siginfo_t* si, void* ctx)
{
    (void)sig; (void)si;
    ucontext_t* uc = (ucontext_t*)ctx;
    uint64_t* x = uc->uc_mcontext->__ss.__x;
    long ret = g_active_dispatch(x[8], x[0], x[1], x[2], x[3], x[4], x[5]);
    x[0] = (uint64_t)ret;
    uc->uc_mcontext->__ss.__pc += 4;      // resume past the brk
    trap_calls++;
}

static void set_dispatch(fastcall_dispatch_fn fn)
{ g_active_dispatch = fn; fastcall_set_dispatch(fn); }

// =============================================================================================
// helpers
// =============================================================================================

static int g_fail;
#define CHECK(cond, ...) do { \
    if (cond) { printf("  ok   " __VA_ARGS__); printf("\n"); } \
    else      { printf("  FAIL " __VA_ARGS__); printf("\n"); g_fail++; } } while (0)

// Emit a blob the way the loader does: rewrite svc->brk in the plain staging buffer, THEN copy
// into JIT.  Returns the final address.
static uint32_t* emit_guest(const uint32_t* insns, int n)
{
    uint32_t staged[256];
    memcpy(staged, insns, (size_t)n * 4);
    gmem_rewrite_exec_insns((unsigned char*)staged, (size_t)n * 4);
    return (uint32_t*)jit_emit(staged, n);
}

static uint64_t now_ns(void)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}

// =============================================================================================

typedef long (*fn6_t)(long, long, long, long, long, long);
typedef long (*fn1_t)(long);

int main(void)
{
    printf("=== fastcall test ===\n");

    g_arena_cap = 256 * 1024;
    g_arena = (unsigned char*)gmem_reserve(g_arena_cap);
    if (!g_arena) return 1;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sigtrap;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGTRAP, &sa, NULL);

    set_dispatch(recording_dispatch);

    unsigned long thunk = fastcall_build_thunk();
    CHECK(thunk != 0, "thunk built at 0x%lx", thunk);
    if (!thunk) return 1;

    // -----------------------------------------------------------------------------------------
    // (A) a guest-like NON-LEAF syscall wrapper: x30 is spilled before the site and reloaded
    //     after it, so x30 is DEAD at the site and it must be patched.
    // -----------------------------------------------------------------------------------------
    printf("\n[A] patchable site (non-leaf wrapper)\n");
    {
        const unsigned NR_A = 0x0101;
        uint32_t code[] = {
            E_stp_pre(29, 30, SP, -16),      // stp x29,x30,[sp,#-16]!
            E_movz(8, NR_A, 0),              // mov x8, #NR_A
            E_svc0(),                        // svc #0     <- rewritten, then patched
            E_ldp_post(29, 30, SP, 16),      // ldp x29,x30,[sp],#16    (DEF of x30)
            E_ret(),                         // ret
        };
        int n = (int)(sizeof code / 4);
        uint32_t* f = emit_guest(code, n);
        CHECK(f[2] == 0xD4200000u, "svc #0 rewritten to brk #0");

        int skipped = -1;
        int patched = fastcall_patch_ex(f, (size_t)n * 4, thunk, &skipped);
        CHECK(patched == 1 && skipped == 0, "patched=%d skipped=%d", patched, skipped);
        CHECK((f[2] & 0xFC000000u) == 0x94000000u, "site is now `bl thunk` (0x%08x)", f[2]);

        long before_disp = disp_calls, before_trap = trap_calls;
        long rv = ((fn6_t)f)(11, 22, 33, 44, 55, 66);

        CHECK(rv == DISPATCH_RET, "caller got 0x%lx (want 0x%lx)", rv, (long)DISPATCH_RET);
        CHECK(r_nr == NR_A, "dispatch saw nr=0x%lx", r_nr);
        CHECK(r_a[0]==11 && r_a[1]==22 && r_a[2]==33 && r_a[3]==44 && r_a[4]==55 && r_a[5]==66,
              "dispatch saw args %lu,%lu,%lu,%lu,%lu,%lu",
              r_a[0],r_a[1],r_a[2],r_a[3],r_a[4],r_a[5]);
        CHECK(disp_calls == before_disp + 1 && trap_calls == before_trap,
              "went through the THUNK, not the trap (dispatch+%ld trap+%ld)",
              disp_calls - before_disp, trap_calls - before_trap);
        CHECK(r_x18 == 0, "host dispatch saw x18 == 0 (got 0x%lx)", r_x18);
    }

    // -----------------------------------------------------------------------------------------
    // (A2) the literal `mov x8,#nr ; svc #0 ; ret` shape — x30 is LIVE (the ret reads it).
    // -----------------------------------------------------------------------------------------
    printf("\n[A2] `mov x8 ; svc ; ret` — x30 live via ret\n");
    {
        uint32_t code[] = { E_movz(8, 0x0202, 0), E_svc0(), E_ret() };
        int n = (int)(sizeof code / 4);
        uint32_t* f = emit_guest(code, n);
        uint32_t before = f[1];
        int skipped = -1;
        int patched = fastcall_patch_ex(f, (size_t)n * 4, thunk, &skipped);
        CHECK(patched == 0 && skipped == 1, "patched=%d skipped=%d", patched, skipped);
        CHECK(f[1] == before && f[1] == 0xD4200000u, "site untouched, still brk #0 (0x%08x)", f[1]);
    }

    // -----------------------------------------------------------------------------------------
    // (B) THE x30 REGRESSION TEST — musl's leaf wrapper:
    //         mov x8,#nr ; svc #0 ; b __syscall_ret
    //     __syscall_ret returns through x30. Patching this to `bl` corrupts the return address
    //     (the observed symptom was lseek spinning forever and hanging `ls -la`).
    // -----------------------------------------------------------------------------------------
    printf("\n[B] x30 REGRESSION: musl leaf idiom must be SKIPPED\n");
    {
        const unsigned NR_B = 0x0303;
        uint32_t code[] = {
            E_movz(8, NR_B, 0),          // 0: mov x8, #NR_B
            E_svc0(),                    // 1: svc #0        <- MUST stay brk #0
            E_b(5),                      // 2: b  __syscall_ret   (index 2+5 = 7)
            0, 0, 0, 0,                  // 3..6: gap (unreachable)
            E_add_imm(0, 0, 7),          // 7: __syscall_ret: add x0, x0, #7
            E_ret(),                     // 8: ret            (returns through the caller's x30)
        };
        int n = (int)(sizeof code / 4);
        uint32_t* f = emit_guest(code, n);
        uint32_t before = f[1];

        int skipped = -1;
        int patched = fastcall_patch_ex(f, (size_t)n * 4, thunk, &skipped);
        CHECK(patched == 0, "patched=%d (must be 0)", patched);
        CHECK(skipped == 1, "skipped=%d (x30-live count)", skipped);
        CHECK(f[1] == before && f[1] == 0xD4200000u,
              "bytes UNCHANGED at the site: 0x%08x (still brk #0)", f[1]);

        long before_trap = trap_calls, before_disp = disp_calls;
        long rv = ((fn6_t)f)(1, 2, 3, 4, 5, 6);   // if it had been patched this never returns
        CHECK(rv == DISPATCH_RET + 7, "leaf wrapper RETURNED, rv=0x%lx (want 0x%lx)",
              rv, (long)DISPATCH_RET + 7);
        CHECK(trap_calls == before_trap + 1 && disp_calls == before_disp + 1,
              "served by the TRAP path (trap+%ld dispatch+%ld)",
              trap_calls - before_trap, disp_calls - before_disp);
        CHECK(r_nr == NR_B, "trap dispatch saw nr=0x%lx", r_nr);
    }

    // -----------------------------------------------------------------------------------------
    // (C) x18 and v0-v31 preserved across a fast syscall.
    //     Guest sets x18 = 0x1234000000005678 and v[i].2d = {0xA000+i, 0xA000+i}, does the
    //     syscall, then dumps v0-v31, x18 and x0 into a buffer.
    // -----------------------------------------------------------------------------------------
    printf("\n[C] x18 + v0-v31 preserved across a fast syscall\n");
    {
        const unsigned NR_C = 0x0404;
        uint32_t code[128];
        int n = 0;
        code[n++] = E_stp_pre(29, 30, SP, -192);  // stp x29,x30,[sp,#-192]!
        code[n++] = E_stp(19, 18, SP, 16);        // stp x19,x18,[sp,#16]
        // v8-v15 are callee-saved for our C caller; this "guest" overwrites them, so spill them.
        code[n++] = E_stp_q(8,  9,  SP, 32);
        code[n++] = E_stp_q(10, 11, SP, 64);
        code[n++] = E_stp_q(12, 13, SP, 96);
        code[n++] = E_stp_q(14, 15, SP, 128);
        code[n++] = E_mov(19, 0);                 // mov x19, x0       (out buffer)
        for (unsigned i = 0; i < 32; i++) {
            code[n++] = E_movz(9, 0xA000u + i, 0);// mov  x9, #0xA000+i
            code[n++] = E_dup_2d(i, 9);           // dup  vi.2d, x9
        }
        code[n++] = E_movz(18, 0x5678, 0);        // mov  x18, #0x5678
        code[n++] = E_movk(18, 0x1234, 3);        // movk x18, #0x1234, lsl #48
        code[n++] = E_movz(8, NR_C, 0);           // mov  x8, #NR_C
        code[n++] = E_movz(0, 0x77, 0);           // mov  x0, #0x77
        code[n++] = E_movz(1, 0x11, 0);
        code[n++] = E_movz(2, 0x22, 0);
        code[n++] = E_movz(3, 0x33, 0);
        code[n++] = E_movz(4, 0x44, 0);
        code[n++] = E_movz(5, 0x55, 0);
        int site = n;
        code[n++] = E_svc0();                     // svc #0   <- patched
        code[n++] = E_str(0, 19, 520);            // str x0,  [x19,#520]
        code[n++] = E_str(18, 19, 512);           // str x18, [x19,#512]
        for (unsigned i = 0; i < 32; i += 2)
            code[n++] = E_stp_q(i, i + 1, 19, (int)i * 16);
        code[n++] = E_movz(0, 0, 0);              // mov x0, #0
        code[n++] = E_ldp_q(8,  9,  SP, 32);
        code[n++] = E_ldp_q(10, 11, SP, 64);
        code[n++] = E_ldp_q(12, 13, SP, 96);
        code[n++] = E_ldp_q(14, 15, SP, 128);
        code[n++] = E_ldp(19, 18, SP, 16);
        code[n++] = E_ldp_post(29, 30, SP, 192);  // DEF of x30 -> site is DEAD
        code[n++] = E_ret();

        uint32_t* f = emit_guest(code, n);
        int skipped = -1;
        int patched = fastcall_patch_ex(f, (size_t)n * 4, thunk, &skipped);
        CHECK(patched == 1 && skipped == 0, "patched=%d skipped=%d", patched, skipped);

        static uint64_t out[66] __attribute__((aligned(16)));
        memset(out, 0xEE, sizeof out);
        long before_disp = disp_calls, before_trap = trap_calls;
        ((fn1_t)f)((long)(uintptr_t)out);
        CHECK(disp_calls == before_disp + 1 && trap_calls == before_trap,
              "took the thunk, not the trap");

        int bad = -1;
        for (unsigned i = 0; i < 32; i++) {
            uint64_t want = 0xA000u + i;
            if (out[i*2] != want || out[i*2+1] != want) { bad = (int)i; break; }
        }
        CHECK(bad < 0, "v0-v31 all preserved%s", bad < 0 ? "" : "");
        if (bad >= 0)
            printf("       v%d = {0x%llx,0x%llx} want 0x%x\n", bad,
                   (unsigned long long)out[bad*2], (unsigned long long)out[bad*2+1], 0xA000u + bad);
        CHECK(out[64] == 0x1234000000005678ULL, "x18 preserved: 0x%llx",
              (unsigned long long)out[64]);
        CHECK((long)out[65] == DISPATCH_RET, "syscall return in x0: 0x%llx",
              (unsigned long long)out[65]);
        CHECK(r_a[0] == 0x77 && r_a[5] == 0x55 && r_nr == NR_C,
              "dispatch args intact (nr=0x%lx a0=0x%lx a5=0x%lx)", r_nr, r_a[0], r_a[5]);
        CHECK(r_x18 == 0, "host dispatch still saw x18 == 0");
        (void)site;
    }

    // -----------------------------------------------------------------------------------------
    // (E) multi-path liveness: x30 is dead only if EVERY path overwrites it first.
    //     Not executed — these blobs only exercise the analysis.
    // -----------------------------------------------------------------------------------------
    printf("\n[E] multi-path liveness (both sides of a conditional branch)\n");
    {
        // one arm rets through x30 => LIVE => skip
        uint32_t mixed[] = {
            E_movz(8, 0x0606, 0),        // 0
            E_svc0(),                    // 1
            E_cbz(0, 5),                 // 2: cbz x0, -> 7
            E_ldp_post(29, 30, SP, 16),  // 3: arm P1 redefines x30
            E_ret(),                     // 4
            0, 0,                        // 5,6
            E_ret(),                     // 7: arm P2 RETURNS through the original x30
        };
        int n = (int)(sizeof mixed / 4);
        uint32_t* f = emit_guest(mixed, n);
        int skipped = -1;
        int patched = fastcall_patch_ex(f, (size_t)n * 4, thunk, &skipped);
        CHECK(patched == 0 && skipped == 1,
              "one arm uses x30 => SKIP (patched=%d skipped=%d)", patched, skipped);

        // both arms redefine x30 => DEAD => patch
        uint32_t both[] = {
            E_movz(8, 0x0707, 0),        // 0
            E_svc0(),                    // 1
            E_cbz(0, 4),                 // 2: cbz x0, -> 6
            E_ldp_post(29, 30, SP, 16),  // 3
            E_ret(),                     // 4
            0,                           // 5
            E_ldp_post(29, 30, SP, 16),  // 6
            E_ret(),                     // 7
        };
        n = (int)(sizeof both / 4);
        f = emit_guest(both, n);
        skipped = -1;
        patched = fastcall_patch_ex(f, (size_t)n * 4, thunk, &skipped);
        CHECK(patched == 1 && skipped == 0,
              "both arms redefine x30 => PATCH (patched=%d skipped=%d)", patched, skipped);
    }

    // -----------------------------------------------------------------------------------------
    // (D) benchmark
    // -----------------------------------------------------------------------------------------
    printf("\n[D] benchmark\n");
    {
        const long N = 1000000;
        uint32_t code[] = {
            E_stp_pre(29, 30, SP, -32),   // 0: stp x29,x30,[sp,#-32]!
            E_stp(19, 20, SP, 16),        // 1: stp x19,x20,[sp,#16]
            E_mov(19, 0),                 // 2: mov x19, x0        (count)
            E_movz(8, 0x0505, 0),         // 3: loop: mov x8,#nr
            E_movz(0, 1, 0),              // 4: mov x0,#1
            E_svc0(),                     // 5: svc #0   <- patched
            E_subs_imm(19, 19, 1),        // 6: subs x19,x19,#1
            E_bne(-4),                    // 7: b.ne loop  (index 7 - 4 = 3)
            E_ldp(19, 20, SP, 16),        // 8:
            E_ldp_post(29, 30, SP, 32),   // 9: DEF of x30
            E_ret(),                      // 10:
        };
        int n = (int)(sizeof code / 4);
        uint32_t* f = emit_guest(code, n);
        int skipped = -1;
        int patched = fastcall_patch_ex(f, (size_t)n * 4, thunk, &skipped);
        CHECK(patched == 1 && skipped == 0,
              "loop body patched (patched=%d skipped=%d)", patched, skipped);

        set_dispatch(bench_dispatch);
        ((fn1_t)f)(10000);                            // warm up
        uint64_t t0 = now_ns();
        ((fn1_t)f)(N);
        uint64_t t1 = now_ns();
        double per = (double)(t1 - t0) / (double)N;
        printf("  fastcall : %10.2f ns/call  (%ld calls in %.3f ms)\n",
               per, N, (double)(t1 - t0) / 1e6);
        printf("  trap path: %10.2f ns/call  (Mach exception round trip, measured previously)\n",
               9700.0);
        printf("  speedup  : %10.0fx\n", 9700.0 / per);
        CHECK(per < 200.0, "fast path is < 200 ns/call");
        set_dispatch(recording_dispatch);
    }

    printf("\n=== %s ===\n", g_fail ? "FAILURES" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
