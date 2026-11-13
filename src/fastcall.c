// fastcall.c — trap-free syscall fast path: guest `svc #0` (already rewritten to `brk #0`)
// becomes `bl <thunk>`, so a Linux syscall runs IN-THREAD as a plain function call.
//
// Cost model:
//   brk #0 -> EXC_BREAKPOINT -> mach_msg -> exception-server thread -> thread_set_state -> resume
//     ~9.7us (cross-thread round trip)
//   bl thunk -> save regs -> swap stack -> blr dispatch -> restore -> ret
//     ~ns (no kernel, no thread hand-off)
//
// The two hard parts are both documented in fastcall.h and are implemented here:
//   1. the thunk must make the host call look like a normal Darwin call (x18 zeroed, clean
//      native stack, full NEON save) while looking like a no-op to the guest (every register
//      except x0 preserved);
//   2. `bl` clobbers x30, so a site may only be patched where x30 is provably DEAD.
//
// See fc_x30_live() for the liveness analysis.

#include "fastcall.h"
#include "gmem.h"

#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

// ---------------------------------------------------------------------------------------------
// State the emitted thunk reaches through absolute addresses baked into the code with movz/movk.
// ---------------------------------------------------------------------------------------------

// Dispatch function pointer. The thunk does `ldr x9,[&g_fc_dispatch]; blr x9`, so it can be
// re-pointed at runtime without re-emitting the thunk.
static fastcall_dispatch_fn g_fc_dispatch;

// Top of the dedicated NATIVE stack the thunk switches to before calling host code.
//
// THREADING CAVEAT (as fastcall.h warns): this is a SINGLE GLOBAL, which is correct only for a
// single-threaded guest. It is deliberately reached through one level of indirection — the thunk
// materializes `&g_fc_native_sp` and does `ldr x9,[x9]` — so growing this to per-thread stacks is
// a localized change: replace that single load with a per-thread lookup (e.g. index a small table
// off the virtualized guest TCB the TLS trap handler already maintains, or point the slot at a
// per-thread struct on clone()). Nothing else in the thunk or the patcher has to change.
static void* g_fc_native_sp;


static unsigned long g_fc_thunk;         // emitted once
static int           g_fc_range_skips;   // diagnostics: sites skipped because bl couldn't reach

void fastcall_set_dispatch(fastcall_dispatch_fn fn) { g_fc_dispatch = fn; }

// ---------------------------------------------------------------------------------------------
// aarch64 instruction encoders
// ---------------------------------------------------------------------------------------------

#define REG_SP 31u
#define REG_ZR 31u

static uint32_t e_sub_imm(unsigned rd, unsigned rn, unsigned imm12)
{ return 0xD1000000u | ((imm12 & 0xFFFu) << 10) | (rn << 5) | rd; }

static uint32_t e_add_imm(unsigned rd, unsigned rn, unsigned imm12)
{ return 0x91000000u | ((imm12 & 0xFFFu) << 10) | (rn << 5) | rd; }

// stp/ldp Xt1,Xt2,[Xn,#off]   (signed offset, off must be a multiple of 8)
static uint32_t e_stp(unsigned t1, unsigned t2, unsigned rn, int off)
{ return 0xA9000000u | (((uint32_t)(off / 8) & 0x7Fu) << 15) | (t2 << 10) | (rn << 5) | t1; }
static uint32_t e_ldp(unsigned t1, unsigned t2, unsigned rn, int off)
{ return 0xA9400000u | (((uint32_t)(off / 8) & 0x7Fu) << 15) | (t2 << 10) | (rn << 5) | t1; }

// st1/ld1 {Vt.2D - Vt+3.2D}, [Xn], #64   (4 vector registers per instruction, post-index)
static uint32_t e_st1_4x2d_post(unsigned vt, unsigned rn)
{ return 0x4C9F2C00u | (rn << 5) | vt; }
static uint32_t e_ld1_4x2d_post(unsigned vt, unsigned rn)
{ return 0x4CDF2C00u | (rn << 5) | vt; }

static uint32_t e_movz(unsigned rd, unsigned imm16, unsigned hw)
{ return 0xD2800000u | (hw << 21) | ((imm16 & 0xFFFFu) << 5) | rd; }
static uint32_t e_movk(unsigned rd, unsigned imm16, unsigned hw)
{ return 0xF2800000u | (hw << 21) | ((imm16 & 0xFFFFu) << 5) | rd; }

// ldr Xt,[Xn,#off]  (unsigned offset, multiple of 8)
static uint32_t e_ldr_imm(unsigned rt, unsigned rn, unsigned off)
{ return 0xF9400000u | (((off / 8) & 0xFFFu) << 10) | (rn << 5) | rt; }

// mov Xd, Xm  == orr Xd, XZR, Xm       (cannot involve SP)
static uint32_t e_mov_reg(unsigned rd, unsigned rm)
{ return 0xAA0003E0u | (rm << 16) | rd; }
// mov Xd, SP / mov SP, Xn  == add Xd, Xn, #0
static uint32_t e_mov_sp(unsigned rd, unsigned rn) { return e_add_imm(rd, rn, 0); }

static uint32_t e_blr(unsigned rn) { return 0xD63F0000u | (rn << 5); }
static uint32_t e_ret_lr(void)     { return 0xD65F03C0u; }

static int emit_imm64(uint32_t* b, int n, unsigned rd, uint64_t v)
{
    b[n++] = e_movz(rd, (unsigned)(v         & 0xFFFF), 0);
    b[n++] = e_movk(rd, (unsigned)((v >> 16) & 0xFFFF), 1);
    b[n++] = e_movk(rd, (unsigned)((v >> 32) & 0xFFFF), 2);
    b[n++] = e_movk(rd, (unsigned)((v >> 48) & 0xFFFF), 3);
    return n;
}

// ---------------------------------------------------------------------------------------------
// The native stack
// ---------------------------------------------------------------------------------------------

#define FC_NATIVE_STACK_BYTES (512u * 1024u)
#define FC_GUARD_BYTES        (16u * 1024u)   // one guest page

// PER-THREAD native stacks. The single global above is correct only for a single-threaded guest;
// two guest threads in a syscall at once would run host code on the same stack and corrupt each
// other. Darwin's TPIDRRO_EL0 is per-thread (masking the low 3 bits, which are the cpu number),
// and a pthread_key_create()'d key k puts this thread's slot at tsd_base + k*8 — exactly the
// mechanism gmem.c already uses for per-thread x18. The thunk reads that slot inline, so the
// switch stays three instructions and costs nothing.
static pthread_key_t g_fc_key;
static unsigned      g_fc_tsd_off;          // 0 = not available; fastcall then refuses to build

static void fc_free_stack(void* p) {
    if (p) munmap((char*)p - (FC_GUARD_BYTES + FC_NATIVE_STACK_BYTES), 
                  FC_GUARD_BYTES + FC_NATIVE_STACK_BYTES);
}

// Give THIS thread a native stack. Must run on every guest thread before it executes any guest
// instruction, because the thunk reads the slot with no null check — three instructions, no
// branch, is the entire point. shim.c calls it from each guest thread entry.
int fastcall_thread_init(void) {
    if (!g_fc_tsd_off) return 0;
    if (pthread_getspecific(g_fc_key)) return 1;
    size_t total = FC_GUARD_BYTES + FC_NATIVE_STACK_BYTES;
    void* p = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return 0;
    mprotect(p, FC_GUARD_BYTES, PROT_NONE);         // guard page under the stack
    uintptr_t top = ((uintptr_t)p + total) & ~(uintptr_t)15;
    pthread_setspecific(g_fc_key, (void*)top);
    return 1;
}

static int fc_alloc_native_stack(void)
{
    if (g_fc_native_sp) return 1;
    size_t total = FC_GUARD_BYTES + FC_NATIVE_STACK_BYTES;
    void* p = mmap(NULL, total, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return 0;
    // Guard page at the low end so a runaway host call faults instead of walking into the heap.
    mprotect(p, FC_GUARD_BYTES, PROT_NONE);
    uintptr_t top = (uintptr_t)p + total;
    top &= ~(uintptr_t)15;                    // AAPCS: SP must stay 16-byte aligned
    g_fc_native_sp = (void*)top;
    return 1;
}

// ---------------------------------------------------------------------------------------------
// fastcall_build_thunk
//
// Entered from guest code by `bl`:  x0..x5 = syscall args, x8 = syscall number,
//                                   x30 = return address (guest site + 4), SP = guest Linux stack.
// Leaves:                           x0 = raw Linux return value, EVERYTHING else unchanged.
//
// Frame (built on the GUEST stack — writing there is fine, only *executing host code* on it is
// not; the host call itself happens on the clean native stack):
//     [sp,#  0] x1,x2      [sp,# 80] x11,x12    [sp,#160] x19,x20
//     [sp,# 16] x3,x4      [sp,# 96] x13,x14    [sp,#176] v0..v31   (512 bytes)
//     [sp,# 32] x5,x6      [sp,#112] x15,x16    total frame 704 bytes (16-aligned)
//     [sp,# 48] x7,x8      [sp,#128] x17,x18
//     [sp,# 64] x9,x10     [sp,#144] x29,x30
// x21..x28 are never touched by the thunk and are preserved by the C callee (AAPCS), so they need
// no slot. x19/x20 get one because x19 carries the guest SP across the call.
// ---------------------------------------------------------------------------------------------

#define FR_SIZE 704
#define FR_NEON 176

// The opt-in gate and the build-once, in ONE place. Both callers need it: elfload.c for the
// images the shim loads itself (the main binary and the interpreter) and gmem.c for everything
// the GUEST maps later through mmap/mprotect -- which is where libc.so.6 lives, and therefore
// where nearly every syscall a real program makes actually is. Patching only the elfload path
// covers ld.so and misses libc entirely.
unsigned long fastcall_thunk_once(void)
{
    static unsigned long thunk;
    static int tried;
    if (tried) return thunk;
    tried = 1;
    // DEFAULT ON, opt OUT with RDFASTCALL=0. It was opt-in while the fast path was unproven;
    // it is now validated against the trap path on the whole guest test suite (x18thr, x18mix,
    // x18fork, thr, forktest, futexto, sysvipc, nstest, closerange, aborttest — identical
    // behaviour, zero guard hits, zero faults), 8 threads x 20000 iters of real file I/O,
    // fork/exec/system/popen, and a Blender Cycles render. The A/B arm is kept because it is the
    // only way to attribute a future regression: RDFASTCALL=0 restores the pure trap path.
    const char* e = getenv("RDFASTCALL");
    if (e && *e == '0') return 0;
    thunk = fastcall_build_thunk();
    if (!thunk) fprintf(stderr, "[fastcall] thunk build FAILED; staying on the trap path\n");
    return thunk;
}

unsigned long fastcall_build_thunk(void)
{
    if (g_fc_thunk) return g_fc_thunk;
    if (!fc_alloc_native_stack()) return 0;
    // Per-thread slot first: without it the thunk cannot be emitted safely at all.
    if (!g_fc_tsd_off) {
        if (pthread_key_create(&g_fc_key, fc_free_stack) != 0) {
            fprintf(stderr, "[fastcall] pthread_key_create failed; staying on the trap path\n");
            return 0;
        }
        g_fc_tsd_off = (unsigned)g_fc_key * 8u;
    }
    if (!fastcall_thread_init()) {
        fprintf(stderr, "[fastcall] no native stack for this thread; staying on the trap path\n");
        return 0;
    }

    static const unsigned char gp[11][2] = {
        {1,2},{3,4},{5,6},{7,8},{9,10},{11,12},{13,14},{15,16},{17,18},{29,30},{19,20}
    };

    uint32_t b[192];
    int n = 0, k, v;

    // ---- prologue: carve the frame out of the guest stack and spill ----
    b[n++] = e_sub_imm(REG_SP, REG_SP, FR_SIZE);            // sub  sp, sp, #704
    for (k = 0; k < 11; k++)
        b[n++] = e_stp(gp[k][0], gp[k][1], REG_SP, k * 16); // stp  xA, xB, [sp,#k*16]

    // Full NEON/FP file: the Linux syscall ABI preserves v0-v31, but the C dispatch will
    // clobber v0-v7/v16-v31 and the upper halves of v8-v15. 8 instructions, not 32.
    b[n++] = e_add_imm(9, REG_SP, FR_NEON);                 // add  x9, sp, #176
    for (v = 0; v < 32; v += 4)
        b[n++] = e_st1_4x2d_post(v, 9);                     // st1  {vN.2d-vN+3.2d}, [x9], #64

    b[n++] = e_mov_sp(19, REG_SP);                          // mov  x19, sp     (guest SP/frame base)

    // ---- marshal Linux ABI -> C ABI: dispatch(nr, a0, a1, a2, a3, a4, a5) ----
    b[n++] = e_mov_reg(6, 5);                               // mov  x6, x5
    b[n++] = e_mov_reg(5, 4);                               // mov  x5, x4
    b[n++] = e_mov_reg(4, 3);                               // mov  x4, x3
    b[n++] = e_mov_reg(3, 2);                               // mov  x3, x2
    b[n++] = e_mov_reg(2, 1);                               // mov  x2, x1
    b[n++] = e_mov_reg(1, 0);                               // mov  x1, x0
    b[n++] = e_mov_reg(0, 8);                               // mov  x0, x8      (syscall number)

    // ---- switch to the clean native stack ----
    // macOS libsystem (malloc, os_unfair_lock, readdir, ...) crashes when run on the guest's
    // Linux stack, so the host call must not execute there.
    b[n++] = 0xD53BD069u;                                   // mrs  x9, tpidrro_el0
    b[n++] = 0x927DF129u;                                   // and  x9, x9, #~7   (drop cpu id)
    b[n++] = 0xF9400000u | (((g_fc_tsd_off / 8) & 0xFFFu) << 10) | (9u << 5) | 9u;
    b[n++] = e_mov_sp(REG_SP, 9);                           // mov  sp, x9

    // ---- x18: Darwin's reserved platform register. Linux code uses it as a GPR; leaving a
    //      guest value in it makes libsystem misbehave or crash. Zero it for the host call.
    b[n++] = e_mov_reg(18, REG_ZR);                         // mov  x18, xzr

    // ---- call the dispatch (loaded from the slot, so set_dispatch works after build) ----
    n = emit_imm64(b, n, 9, (uint64_t)(uintptr_t)&g_fc_dispatch);
    b[n++] = e_ldr_imm(9, 9, 0);                            // ldr  x9, [x9]
    b[n++] = e_blr(9);                                      // blr  x9          (x0 = result)

    // ---- back onto the guest stack and unspill (x0 deliberately NOT restored) ----
    b[n++] = e_mov_sp(REG_SP, 19);                          // mov  sp, x19
    b[n++] = e_add_imm(9, REG_SP, FR_NEON);                 // add  x9, sp, #176
    for (v = 0; v < 32; v += 4)
        b[n++] = e_ld1_4x2d_post(v, 9);                     // ld1  {vN.2d-vN+3.2d}, [x9], #64
    for (k = 0; k < 11; k++)
        b[n++] = e_ldp(gp[k][0], gp[k][1], REG_SP, k * 16); // ldp  xA, xB, [sp,#k*16]
    b[n++] = e_add_imm(REG_SP, REG_SP, FR_SIZE);            // add  sp, sp, #704
    b[n++] = e_ret_lr();                                    // ret              (x30 = site+4)

    size_t bytes = (size_t)n * 4;
    void* code = gmem_reserve(bytes);
    if (!code) return 0;
    if (gmem_map_code(code, b, bytes) != 0) return 0;

    g_fc_thunk = (unsigned long)(uintptr_t)code;
    return g_fc_thunk;
}

// ---------------------------------------------------------------------------------------------
// x30 liveness
//
// A site may be rewritten to `bl` only if the value in x30 at that site is never read again, on
// ANY path, before something overwrites it. This is a plain forward may-liveness walk:
//
//   USE  of x30 (a store of x30, `ret`, an indirect `br`, `blr x30`, x30 as an operand) => LIVE
//   DEF  of x30 (`bl`, `blr xN`, `ldp x29,x30,[sp],#..`, `mov x30,..`, `adr x30,..`)    => this
//                                                                          path is finished, DEAD
//   anything unrecognized / unresolvable / off the end of the image                     => LIVE
//
// Liveness is a LEAST fixed point ("there exists a path to a use"), so revisiting a node adds no
// information and the walk can simply stop there — that is what makes the loop in a bench/retry
// idiom terminate correctly rather than conservatively.
// ---------------------------------------------------------------------------------------------

enum { FC_FALL, FC_JMP, FC_BOTH, FC_LIVE, FC_DEAD };
typedef struct { int kind; long target; } fc_step;

static long fc_sext(uint32_t v, int bits)
{
    long s = (long)(uint64_t)v;
    if (s & (1L << (bits - 1))) s -= (1L << bits);
    return s;
}

// Classify one instruction at index `i` with respect to x30.
static fc_step fc_classify(uint32_t in, long i)
{
    fc_step r; r.kind = FC_FALL; r.target = 0;

    unsigned rt  =  in        & 31u;   // Rt  / Rd
    unsigned rn  = (in >>  5) & 31u;   // Rn
    unsigned rt2 = (in >> 10) & 31u;   // Rt2 / Ra
    unsigned rm  = (in >> 16) & 31u;   // Rm  / Rs
    unsigned grp = (in >> 25) & 0xFu;  // op0, bits 28:25

    // ------------------------------------------------------------------ branch / exception / system
    if (grp == 0xA || grp == 0xB) {
        if ((in & 0x7C000000u) == 0x14000000u) {            // B / BL (imm26)
            if (in & 0x80000000u) { r.kind = FC_DEAD; return r; }        // BL writes x30
            r.kind = FC_JMP; r.target = i + fc_sext(in & 0x3FFFFFFu, 26);
            return r;
        }
        if ((in & 0xFE000000u) == 0x54000000u) {            // B.cond / BC.cond
            r.kind = FC_BOTH; r.target = i + fc_sext((in >> 5) & 0x7FFFFu, 19);
            return r;
        }
        if ((in & 0x7E000000u) == 0x34000000u) {            // CBZ / CBNZ
            if (rt == 30) { r.kind = FC_LIVE; return r; }
            r.kind = FC_BOTH; r.target = i + fc_sext((in >> 5) & 0x7FFFFu, 19);
            return r;
        }
        if ((in & 0x7E000000u) == 0x36000000u) {            // TBZ / TBNZ
            if (rt == 30) { r.kind = FC_LIVE; return r; }
            r.kind = FC_BOTH; r.target = i + fc_sext((in >> 5) & 0x3FFFu, 14);
            return r;
        }
        if ((in & 0xFF000000u) == 0xD4000000u) {            // exception generation
            unsigned opc = (in >> 21) & 7u, ll = in & 3u;
            if (opc == 0 && ll == 1) { r.kind = FC_FALL; return r; }     // SVC  (preserves x30)
            if (opc == 1 && ll == 0) { r.kind = FC_FALL; return r; }     // BRK  (trap, preserves x30)
            r.kind = FC_LIVE; return r;                                  // HLT/HVC/SMC/...
        }
        if ((in & 0xFFC00000u) == 0xD5000000u) {            // system
            if ((in & 0xFFF00000u) == 0xD5300000u) {        // MRS Xt, sysreg  -> defines Xt
                r.kind = (rt == 30) ? FC_DEAD : FC_FALL; return r;
            }
            // MSR sysreg,Xt / hints / barriers / SYS: Xt (or reg 31) is a source.
            r.kind = (rt == 30) ? FC_LIVE : FC_FALL; return r;
        }
        if ((in & 0xFE000000u) == 0xD6000000u) {            // unconditional branch (register)
            if ((in & 0xFFFFFC1Fu) == 0xD65F0000u) { r.kind = FC_LIVE; return r; }   // RET Xn
            if ((in & 0xFFFFFC1Fu) == 0xD61F0000u) { r.kind = FC_LIVE; return r; }   // BR  Xn (target unknown)
            if ((in & 0xFFFFFC1Fu) == 0xD63F0000u) {                                  // BLR Xn
                r.kind = (rn == 30) ? FC_LIVE : FC_DEAD; return r;
            }
            r.kind = FC_LIVE; return r;                     // RETAA/BRAA/ERET/...
        }
        r.kind = FC_LIVE; return r;
    }

    // ------------------------------------------------------------------ cheap universal reject
    // Every base-ISA register operand lives in bits 4:0, 9:5, 14:10 or 20:16. If none of those
    // fields holds register number 30, the instruction can neither read nor write x30. (BL/BLR,
    // the only instructions with an implicit x30 def, were already handled above.)
    if (rt != 30 && rn != 30 && rt2 != 30 && rm != 30) { r.kind = FC_FALL; return r; }

    // ------------------------------------------------------------------ data processing -- immediate
    if (grp == 0x8 || grp == 0x9) {
        if ((in & 0x1F000000u) == 0x10000000u) {            // ADR / ADRP  (Rd only; 9:5 is imm)
            r.kind = (rt == 30) ? FC_DEAD : FC_FALL; return r;
        }
        if ((in & 0x1F000000u) == 0x11000000u) {            // ADD/SUB (immediate), ADDG/SUBG
            if (rn == 30) { r.kind = FC_LIVE; return r; }
            r.kind = (rt == 30) ? FC_DEAD : FC_FALL; return r;
        }
        if ((in & 0x1F800000u) == 0x12000000u) {            // logical (immediate)
            if (rn == 30) { r.kind = FC_LIVE; return r; }
            r.kind = (rt == 30) ? FC_DEAD : FC_FALL; return r;
        }
        if ((in & 0x1F800000u) == 0x12800000u) {            // MOVN / MOVZ / MOVK
            if (rt != 30) { r.kind = FC_FALL; return r; }
            r.kind = (((in >> 29) & 3u) == 3u) ? FC_LIVE : FC_DEAD;  // MOVK merges => a use
            return r;
        }
        if ((in & 0x1F800000u) == 0x13000000u) {            // bitfield
            if (rn == 30) { r.kind = FC_LIVE; return r; }
            if (rt != 30) { r.kind = FC_FALL; return r; }
            r.kind = (((in >> 29) & 3u) == 1u) ? FC_LIVE : FC_DEAD;  // BFM merges => a use
            return r;
        }
        if ((in & 0x1F800000u) == 0x13800000u) {            // EXTR
            if (rn == 30 || rm == 30) { r.kind = FC_LIVE; return r; }
            r.kind = (rt == 30) ? FC_DEAD : FC_FALL; return r;
        }
        r.kind = FC_LIVE; return r;
    }

    // ------------------------------------------------------------------ loads and stores (x1x0)
    if ((grp & 0x5u) == 0x4u) {
        unsigned V = (in >> 26) & 1u;

        if ((in & 0x3A000000u) == 0x28000000u) {            // load/store PAIR (all index modes)
            unsigned L = (in >> 22) & 1u;
            if (rn == 30) { r.kind = FC_LIVE; return r; }   // base register is read
            if (V)        { r.kind = FC_FALL; return r; }   // FP pair: Rt/Rt2 name V regs
            if (rt == 30 || rt2 == 30) {
                // L=1 -> ldp x29,x30,[sp],#16  (the epilogue: a DEF, site is patchable)
                // L=0 -> stp x29,x30,[sp,#..]  (the leaf/tail-call idiom: a USE, site is NOT)
                r.kind = L ? FC_DEAD : FC_LIVE; return r;
            }
            r.kind = FC_FALL; return r;
        }
        if ((in & 0x3B000000u) == 0x18000000u) {            // LDR (literal); 9:5 is imm19
            if (V) { r.kind = FC_FALL; return r; }
            r.kind = (rt == 30) ? FC_DEAD : FC_FALL; return r;
        }
        if ((in & 0x3B000000u) == 0x39000000u) {            // ldr/str, unsigned immediate offset
            if (rn == 30) { r.kind = FC_LIVE; return r; }
            if (V)        { r.kind = FC_FALL; return r; }
            if (rt == 30) { r.kind = (((in >> 22) & 3u) == 0u) ? FC_LIVE : FC_DEAD; return r; }
            r.kind = FC_FALL; return r;
        }
        if ((in & 0x3B000000u) == 0x38000000u) {            // unscaled / pre / post / register offset
            unsigned op21 = (in >> 21) & 1u;
            unsigned opt  = (in >> 10) & 3u;
            if (rn == 30) { r.kind = FC_LIVE; return r; }
            if (op21 && opt != 2u) { r.kind = FC_LIVE; return r; }  // atomics/LDAPR: be conservative
            if (op21 && rm == 30)  { r.kind = FC_LIVE; return r; }  // register offset: Rm is read
            if (V)        { r.kind = FC_FALL; return r; }
            if (rt == 30) { r.kind = (((in >> 22) & 3u) == 0u) ? FC_LIVE : FC_DEAD; return r; }
            r.kind = FC_FALL; return r;
        }
        r.kind = FC_LIVE; return r;   // exclusives, SIMD ld/st structures: fail safe
    }

    // ------------------------------------------------------------------ data processing -- register
    if ((grp & 0x7u) == 0x5u) {
        if ((in & 0x1F000000u) == 0x1B000000u) {            // 3-source (MADD/MSUB/...): Ra at 14:10
            if (rn == 30 || rm == 30 || rt2 == 30) { r.kind = FC_LIVE; return r; }
            r.kind = (rt == 30) ? FC_DEAD : FC_FALL; return r;
        }
        if (rn == 30 || rm == 30) { r.kind = FC_LIVE; return r; }
        r.kind = (rt == 30) ? FC_DEAD : FC_FALL; return r;  // includes `mov x30,xN` (orr)
    }

    // ------------------------------------------------------------------ SIMD/FP data processing
    // Only the GP<->vector movers (fmov/ins/umov/dup/cvt) can touch x30 and the direction is
    // awkward to decode. Reached only when 30 appears in a field at all, so fail safe.
    r.kind = FC_LIVE; return r;
}

#define FC_MAX_STEPS 64    // instruction budget per path
#define FC_MAX_VISIT 512   // total nodes explored per site

// 1 => x30 is (or may be) live at `site` and the site must NOT be patched.
static int fc_x30_live(const uint32_t* c, long n, long site)
{
    long visited[FC_MAX_VISIT];
    struct { long idx; int steps; } wl[FC_MAX_VISIT];
    int nv = 0, nw = 0;

    wl[nw].idx = site + 1; wl[nw].steps = 0; nw++;

    while (nw > 0) {
        nw--;
        long i    = wl[nw].idx;
        int  steps = wl[nw].steps;

        for (;;) {
            if (i < 0 || i >= n) return 1;              // ran off the image: unresolvable
            if (steps++ >= FC_MAX_STEPS) return 1;      // budget exhausted: fail safe

            int seen = 0;
            for (int k = 0; k < nv; k++) if (visited[k] == i) { seen = 1; break; }
            if (seen) break;                            // least fixed point: no new information
            if (nv >= FC_MAX_VISIT) return 1;
            visited[nv++] = i;

            fc_step s = fc_classify(c[i], i);
            if (s.kind == FC_LIVE) return 1;
            if (s.kind == FC_DEAD) break;               // this path overwrites x30 first
            if (s.kind == FC_FALL) { i = i + 1; continue; }
            if (s.kind == FC_JMP)  { i = s.target; continue; }

            if (nw >= FC_MAX_VISIT) return 1;           // FC_BOTH
            wl[nw].idx = i + 1; wl[nw].steps = steps; nw++;
            i = s.target;
        }
    }
    return 0;   // every path redefines x30 before any use => safe to clobber it
}

// ---------------------------------------------------------------------------------------------
// fastcall_patch_ex
// ---------------------------------------------------------------------------------------------

#define BRK_0 0xD4200000u   // brk #0 == the already-rewritten `svc #0`

// The DSO case patches a STAGING buffer whose final home is elsewhere, so the `bl` displacement
// must be computed against the address the code will RUN at, not where it currently sits. `va`
// is that final address of code[0]. When va equals the buffer address this is the in-place case
// and the write goes through gmem_map_code (which owns the JIT write-protect toggle and the
// icache flush); otherwise the buffer is ordinary memory and a plain store is correct.
// SYSCALLS THAT MUST NEVER BE PATCHED, wherever the patch is applied from.
//
// The thunk supports exactly one shape: compute a value and return. These do not.
//   220 clone    do_clone() needs the register file the thunk cannot supply
//    93 exit     ends the thread via pthread_exit(), which UNWINDS -- and the thunk has already
//                switched SP to its private native stack, so the unwinder walks the wrong stack
//   221 execve   replaces the image / finishes a vfork child
//   260 wait4    blocks on a CHILD PROCESS
//    95 waitid   ditto
//
// This lives HERE rather than in the caller because there are two callers -- elfload.c for the
// images the shim loads, gmem.c for everything the guest maps -- and having the rule in only one
// of them is exactly the bug this fixes: the guest test binaries are patched by elfload.c, whose
// patcher checked x30 liveness and nothing else, so their exit(93) reached the fast path and the
// runtime guard had to refuse it (-ENOSYS), hanging every threaded test.
//
// glibc and zig both set the number with an immediate in the few instructions before the svc.
// Scanning back stops at any branch: past one, x8 may come from anywhere, and a site whose number
// cannot be proven is left on the trap path rather than guessed at.
int fastcall_denied_site(const void* code, long nwords, long idx)
{
    const uint32_t* c = (const uint32_t*)code;
    for (long b = 1; b <= 8 && idx - b >= 0; b++) {
        uint32_t u = c[idx - b];
        if ((u & 0xFC000000u) == 0x14000000u ||          // b
            (u & 0xFE000000u) == 0xD6000000u ||          // br / ret
            (u & 0xFF000000u) == 0x54000000u) break;     // b.cond
        int movz32 = (u & 0xFFE0001Fu) == (0x52800000u | 8u);   // movz w8, #imm16
        int movz64 = (u & 0xFFE0001Fu) == (0xD2800000u | 8u);   // movz x8, #imm16
        if (movz32 || movz64) {
            unsigned nr = (u >> 5) & 0xFFFFu;
            return (nr == 220u || nr == 93u || nr == 221u || nr == 260u || nr == 95u);
        }
    }
    return 0;
}

static int fc_patch_core(void* code, size_t len, unsigned long va,
                         unsigned long thunk, int* out_skipped)
{
    if (out_skipped) *out_skipped = 0;
    if (!code || !thunk || len < 4) return 0;

    const uint32_t* c = (const uint32_t*)code;
    long n = (long)(len / 4);

    // Pass 1: decide, reading the image in its ORIGINAL (all-brk) state. That is sound: a `brk`
    // preserves x30, so treating a not-yet-patched neighbouring site as a pass-through is the
    // conservative direction — patching it later only ADDS a def, which can never turn a
    // "dead" verdict into a "live" one.
    long  cap = 64, cnt = 0;
    long* hits = (long*)malloc((size_t)cap * sizeof(long));
    if (!hits) return 0;

    int skipped = 0;
    for (long i = 0; i < n; i++) {
        if (c[i] != BRK_0) continue;

        if (fastcall_denied_site(c, n, i)) { skipped++; continue; }
        if (fc_x30_live(c, n, i)) { skipped++; continue; }

        // `bl` reaches +-128MB; anything further stays on the trap path.
        long site  = (long)(va + (unsigned long)i * 4);
        long delta = (long)thunk - site;
        if ((delta & 3) || delta < -(1L << 27) || delta >= (1L << 27)) {
            g_fc_range_skips++;
            continue;
        }

        if (cnt == cap) {
            long  ncap = cap * 2;
            long* nh   = (long*)realloc(hits, (size_t)ncap * sizeof(long));
            if (!nh) { free(hits); if (out_skipped) *out_skipped = skipped; return (int)cnt; }
            hits = nh; cap = ncap;
        }
        hits[cnt++] = i;
    }

    // Pass 2: write. Never memcpy into JIT memory directly — gmem_map_code owns the per-thread
    // write-protect toggle and the icache invalidation; the bracket coalesces both.
    int inplace = (va == (unsigned long)(uintptr_t)code);
    if (inplace) gmem_begin_load();
    long done = 0;
    for (long h = 0; h < cnt; h++) {
        long i = hits[h];
        long delta = (long)thunk - (long)(va + (unsigned long)i * 4);
        uint32_t bl = 0x94000000u | ((uint32_t)((delta >> 2)) & 0x03FFFFFFu);
        if (inplace) { if (gmem_map_code((void*)&c[i], &bl, 4) == 0) done++; }
        else         { memcpy((void*)&c[i], &bl, 4); done++; }
    }
    if (inplace) gmem_end_load();

    free(hits);
    if (out_skipped) *out_skipped = skipped;
    return (int)done;
}

int fastcall_patch_ex(void* code, size_t len, unsigned long thunk, int* out_skipped)
{
    return fc_patch_core(code, len, (unsigned long)(uintptr_t)code, thunk, out_skipped);
}

int fastcall_patch_va(void* code, size_t len, unsigned long va,
                      unsigned long thunk, int* out_skipped)
{
    return fc_patch_core(code, len, va, thunk, out_skipped);
}
