// gmem.c — guest address-space / W^X JIT memory manager.
//
// Apple Silicon has NO RWX, even with entitlements. Every byte of guest memory that will ever be
// EXECUTED must live in a MAP_JIT mapping, whose writability is toggled *per thread* with
// pthread_jit_write_protect_np(). MAP_JIT cannot be combined with MAP_FIXED, so JIT regions are
// always kernel-placed and we keep a table of them; MAP_FIXED requests from the guest are then
// resolved against that table.
//
// Two rules drive the whole file:
//   1. Nobody writes JIT memory directly — everything goes through gmem_map_code(), which toggles
//      write protection on THIS thread and invalidates the icache.
//   2. A pread() can NEVER target JIT memory: the kernel's copyout path ignores the per-thread
//      APRR write-protect toggle, so the copy silently fails (or faults). File bytes are always
//      staged through a plain heap buffer and then gmem_map_code()'d in.
//
// The guest thread runs write-protect(1) (text executable there); the loader and the Mach
// exception-server thread do the writing.
#include "gmem.h"
#include "fastcall.h"
#include "guest_range_ledger.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

// ---------------------------------------------------------------------------
// constants
// ---------------------------------------------------------------------------

#define GPAGE            16384UL          // guest (and host, on Apple Silicon) page size

// LINUX mmap flag values (they differ from macOS and must be translated).
#define LMAP_SHARED      0x01
#define LMAP_PRIVATE     0x02
#define LMAP_FIXED       0x10
#define LMAP_ANONYMOUS   0x20

// LINUX errno values we can produce here (macOS diverges from ~11 and above ~34).
#define LEPERM    1
#define LENOENT   2
#define LEBADF    9
#define LENOMEM  12
#define LEACCES  13
#define LEINVAL  22

// aarch64 encodings we intercept (see ARCHITECTURE.md "Instruction interception").
#define INSN_SVC0        0xD4000001u      // svc #0
#define INSN_MSR_TPIDR   0xD51BD040u      // msr tpidr_el0, xN   (| rt)
#define INSN_MRS_TPIDR   0xD53BD040u      // mrs xN, tpidr_el0   (| rt)
// mrs xN, ctr_el0 — reading the Cache Type Register is LEGAL at EL0 on Linux (the kernel traps
// and emulates it) but SIGILLs on Darwin, which leaves SCTLR_EL1.UCT clear. libgcc's
// __clear_cache() reads it to learn the cache line size, and LLVM calls __clear_cache after every
// JIT emission — so any guest that JITs dies on the first shader it compiles. Verified natively
// on this machine: `mrs x0, ctr_el0` faults while `dc cvau` / `ic ivau` execute fine, so only the
// register read needs virtualizing.
#define INSN_MRS_CTR     0xD53B0020u      // mrs xN, ctr_el0     (| rt)
// mrs xN, DCZID_EL0 — reads the Data Cache Zero ID register. musl's memset reads it to learn the
// DC ZVA granule and only uses `dc zva` when bits[3:0] == 4 (a 64-byte line). It is trapped so the
// answer is guaranteed to agree with the 64-byte granule the dc-zva trap emulates below, even on a
// host whose DCZID_EL0 is unreadable at EL0 or reports a different granule.
#define INSN_MRS_DCZID   0xD53B00E0u      // mrs xN, DCZID_EL0    (| rt)
// dc zva, xN — Data Cache Zero by VA: zeroes ONE 64-byte cache line at Xn. Legal at EL0 on Linux
// (SCTLR_EL1.DZE set) but SIGILLs on Darwin, which leaves DZE clear, so it must be trapped and the
// 64 bytes zeroed host-side. musl's memset calls it in a loop, x3 advancing by 0x40 each iteration.
#define INSN_DC_ZVA      0xD50B7420u      // dc zva, xN           (| rt)
#define INSN_MASK_RT     0xFFFFFFE0u
#define BRK_IMM(imm)     (0xD4200000u | (((unsigned int)(imm) & 0xFFFFu) << 5))
#define BRK_TLS_WRITE    0x100            // brk #(0x100+rt)  == msr tpidr_el0, x<rt>
#define BRK_TLS_READ     0x200            // brk #(0x200+rt)  == mrs x<rt>, tpidr_el0
#define BRK_CTR_READ     0x400            // brk #(0x400+rt)  == mrs x<rt>, ctr_el0
#define BRK_DC_ZVA       0x800            // brk #(0x800+rt)  == dc zva, x<rt>
#define BRK_MRS_DCZID    0x900            // brk #(0x900+rt)  == mrs x<rt>, DCZID_EL0

#define PAGE_DOWN(x)     ((uintptr_t)(x) & ~(uintptr_t)(GPAGE - 1))
#define PAGE_UP(x)       PAGE_DOWN((uintptr_t)(x) + GPAGE - 1)

// ---------------------------------------------------------------------------
// tracked JIT region table
// ---------------------------------------------------------------------------

typedef struct { uintptr_t base; size_t len; } gm_region;

static gm_region*      g_reg;
static int             g_nreg, g_creg;
static pthread_mutex_t g_mu;
static pthread_once_t  g_once = PTHREAD_ONCE_INIT;
static int             g_verbose;

static void gm_once_init(void) {
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);   // gmem_mmap() -> gmem_map_code()
  pthread_mutex_init(&g_mu, &a);
  pthread_mutexattr_destroy(&a);
  g_verbose = getenv("RDVERBOSE") != 0;
}
static void gm_lock(void)   { pthread_once(&g_once, gm_once_init); pthread_mutex_lock(&g_mu); }
static void gm_unlock(void) { pthread_mutex_unlock(&g_mu); }

void gmem_init(void) { pthread_once(&g_once, gm_once_init); }   // idempotent

// caller holds the lock
static int reg_room(void) {
  if (g_nreg < g_creg) return 1;
  int nc = g_creg ? g_creg * 2 : 32;
  gm_region* p = (gm_region*)realloc(g_reg, (size_t)nc * sizeof *p);
  if (!p) return 0;
  g_reg = p; g_creg = nc;
  return 1;
}
static int reg_add(uintptr_t base, size_t len) {           // caller holds the lock
  if (!reg_room()) return 0;
  g_reg[g_nreg].base = base; g_reg[g_nreg].len = len; g_nreg++;
  return 1;
}
// index of a tracked region that FULLY contains [lo,hi), or -1
static int reg_containing(uintptr_t lo, uintptr_t hi) {    // caller holds the lock
  for (int i = 0; i < g_nreg; i++)
    if (lo >= g_reg[i].base && hi <= g_reg[i].base + g_reg[i].len) return i;
  return -1;
}
// index of the LOWEST-based tracked region overlapping [lo,hi), or -1. (Lowest-based matters for
// gmem_mprotect, which walks the range and must not step over a region it has not seen yet.)
static int reg_overlapping(uintptr_t lo, uintptr_t hi) {   // caller holds the lock
  int best = -1;
  for (int i = 0; i < g_nreg; i++)
    if (lo < g_reg[i].base + g_reg[i].len && hi > g_reg[i].base)
      if (best < 0 || g_reg[i].base < g_reg[best].base) best = i;
  return best;
}
// Remove [lo,hi) from the tracked set: wholly-covered regions are dropped, partially covered ones
// are trimmed, and a hole punched in the middle splits the region in two. Used both when a
// MAP_FIXED writable mapping replaces JIT pages with plain RW and when the guest unmaps.
static void reg_untrack(uintptr_t lo, uintptr_t hi) {      // caller holds the lock
  if (hi <= lo) return;
  for (int i = 0; i < g_nreg; ) {
    uintptr_t rlo = g_reg[i].base, rhi = rlo + g_reg[i].len;
    if (hi <= rlo || lo >= rhi) { i++; continue; }          // disjoint
    if (lo <= rlo && hi >= rhi) {                           // wholly covered -> drop
      g_reg[i] = g_reg[--g_nreg];                           // re-examine slot i
      continue;
    }
    if (lo > rlo && hi < rhi) {                             // hole in the middle -> split
      if (reg_room()) {
        g_reg[g_nreg].base = hi; g_reg[g_nreg].len = (size_t)(rhi - hi); g_nreg++;
        g_reg[i].len = (size_t)(lo - rlo);
      } else {
        g_reg[i].len = (size_t)(lo - rlo);                  // OOM: keep the head only
      }
      i++; continue;
    }
    if (lo <= rlo) { g_reg[i].base = hi; g_reg[i].len = (size_t)(rhi - hi); }  // trim head
    else           { g_reg[i].len = (size_t)(lo - rlo); }                      // trim tail
    i++;
  }
}

// ---------------------------------------------------------------------------
// per-thread bulk-load bracket (write-protect batching + icache coalescing)
//
// pthread_jit_write_protect_np() is PER THREAD, so the bracket state must be per thread too —
// a global counter would let one thread's gmem_end_load() re-protect while another is mid-copy.
// ---------------------------------------------------------------------------

#define GM_IC_MAX 32
typedef struct { uintptr_t lo, hi; } gm_range;

static _Thread_local int      t_depth;
static _Thread_local gm_range t_ic[GM_IC_MAX];
static _Thread_local int      t_ic_n;

static void ic_flush(void) {
  for (int i = 0; i < t_ic_n; i++)
    sys_icache_invalidate((void*)t_ic[i].lo, (size_t)(t_ic[i].hi - t_ic[i].lo));
  t_ic_n = 0;
}
// Record [lo,hi) for later invalidation. Absorb every pending range that is contiguous with or
// overlaps it (repeating, since growing the union can bridge ranges already passed over), then
// store the single fused range. Consecutive segments of one image collapse to one invalidation.
static void ic_note(uintptr_t lo, uintptr_t hi) {
  if (hi <= lo) return;
  for (int again = 1; again; ) {
    again = 0;
    for (int i = 0; i < t_ic_n; ) {
      if (t_ic[i].lo > hi || t_ic[i].hi < lo) { i++; continue; }   // neither touching nor overlapping
      if (t_ic[i].lo < lo) lo = t_ic[i].lo;
      if (t_ic[i].hi > hi) hi = t_ic[i].hi;
      t_ic[i] = t_ic[--t_ic_n];                                   // absorbed; re-examine slot i
      again = 1;
    }
  }
  if (t_ic_n == GM_IC_MAX) ic_flush();                            // table full: flush, start over
  t_ic[t_ic_n].lo = lo; t_ic[t_ic_n].hi = hi; t_ic_n++;
}

void gmem_begin_load(void) {
  if (t_depth++ == 0) pthread_jit_write_protect_np(0);
}
void gmem_end_load(void) {
  if (t_depth == 0) return;                                 // unbalanced: ignore
  if (--t_depth == 0) {
    pthread_jit_write_protect_np(1);
    ic_flush();
  }
}

// ---------------------------------------------------------------------------
// reusable staging buffer (one per thread, realloc-grown — never malloc/free per segment)
// ---------------------------------------------------------------------------

static _Thread_local unsigned char* t_stage;
static _Thread_local size_t         t_stage_cap;

static unsigned char* stage_get(size_t n) {
  if (n == 0) return t_stage;
  if (n > t_stage_cap) {
    size_t nc = t_stage_cap ? t_stage_cap : 65536;
    while (nc < n) nc *= 2;
    unsigned char* p = (unsigned char*)realloc(t_stage, nc);
    if (!p) return NULL;
    t_stage = p; t_stage_cap = nc;
  }
  return t_stage;
}

// ---------------------------------------------------------------------------
// host errno -> LINUX errno
//
// Kept local (and static) so gmem.c links standalone; macOS matches Linux for 1..34 EXCEPT that
// 11/35 are swapped (macOS 11=EDEADLK, 35=EAGAIN; Linux 11=EAGAIN, 35=EDEADLK).
// ---------------------------------------------------------------------------
static int gm_lerrno(int he) {
  switch (he) {
    case EAGAIN:       return 11;    // macOS 35
    case EDEADLK:      return 35;    // macOS 11
    case ENAMETOOLONG: return 36;
    case ENOLCK:       return 37;
    case ENOSYS:       return 38;
    case ENOTEMPTY:    return 39;
    case ELOOP:        return 40;
    case EIDRM:        return 43;
    case ENODATA:      return 61;
    case EBADMSG:      return 74;
    case EOVERFLOW:    return 75;
    case ENOTSUP:      return 95;    // == EOPNOTSUPP on macOS
    case ETIMEDOUT:    return 110;
    case ECANCELED:    return 125;
    default: break;
  }
  if (he > 0 && he <= 34) return he;
  return LEINVAL;
}
static long gm_fail(void) { return -(long)gm_lerrno(errno); }

// ---------------------------------------------------------------------------
// JIT reservation + code publication
// ---------------------------------------------------------------------------

void* gmem_reserve(size_t len) {
  if (len == 0 || len > SIZE_MAX - GPAGE) return NULL;
  size_t rlen = (size_t)PAGE_UP(len);
  // MAP_JIT is kernel-placed by definition: it cannot be combined with MAP_FIXED.
  void* p = mmap(NULL, rlen, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_ANON | MAP_PRIVATE | MAP_JIT, -1, 0);
  if (p == MAP_FAILED) {
    fprintf(stderr, "[gmem] MAP_JIT reserve of %zu bytes failed: %s "
                    "(is the binary signed with com.apple.security.cs.allow-jit?)\n",
            rlen, strerror(errno));
    return NULL;
  }
  gm_lock();
  int ok = reg_add((uintptr_t)p, rlen);
  gm_unlock();
  if (!ok) { munmap(p, rlen); return NULL; }
  if (g_verbose) fprintf(stderr, "[gmem] reserve jit %p +%#zx\n", p, rlen);
  return p;
}

int gmem_map_code(void* addr, const void* bytes, size_t len) {
  if (len == 0) return 0;
  if (!addr || !bytes) return -1;
  uintptr_t lo = (uintptr_t)addr, hi = lo + len;
  if (hi < lo) return -1;

  gm_lock();
  int idx = reg_containing(lo, hi);
  gm_unlock();
  if (idx < 0) {
    if (g_verbose)
      fprintf(stderr, "[gmem] map_code %p +%#zx is NOT inside a tracked JIT region\n", addr, len);
    return -1;                                  // refuse: these pages can never be made executable
  }

  if (t_depth > 0) {
    memcpy(addr, bytes, len);                   // write protection already off for this thread
    ic_note(lo, hi);                            // invalidation deferred to gmem_end_load()
  } else {
    pthread_jit_write_protect_np(0);
    memcpy(addr, bytes, len);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(addr, len);
  }
  return 0;
}

// ---------------------------------------------------------------------------
// instruction rewriting (runs on the PLAIN staging buffer, before it reaches JIT)
// ---------------------------------------------------------------------------

// ===========================================================================================
// x18 neutralisation
//
// macOS OWNS x18. It is "the platform register": Apple's ABI says its value is reserved, and XNU
// enforces that by writing 0 into it on EVERY exception return to EL0 -- i.e. on every timer tick
// and every interrupt, asynchronously, roughly 1500 times a second on a running thread
// (tests/repro/x18test.c measures this on the host; tests/repro/x18guest.c measures it from
// inside the guest: 304 clobbers in one busy loop).
//
// Linux aarch64 has no such rule. GCC allocates x18 as an ordinary caller-saved temporary and
// happily keeps a value live in it for dozens of instructions: busybox's SHA-256 round loads the
// state word `h` into w18 at .text+0xaa158 and does not consume it until .text+0xaa1f0, ~40
// instructions later. Any interrupt inside that window silently substitutes 0 for `h`. That is
// the whole story behind "sha256sum/md5sum return a different wrong digest on nearly every run,
// while cksum -- whose CRC loop never allocates x18 -- is always right", and behind the failure
// rate scaling linearly with the number of bytes hashed.
//
// The register is therefore unusable by the guest, and the guest's own code needs every other
// register, so x18's value is moved OUT of the register file into memory (gmem_guest_x18). Every
// instruction that mentions x18 is replaced 1:1 by `b <veneer>` -- an unconditional branch is the
// only rewrite that clobbers nothing -- and the out-of-line veneer re-executes it with x18 renamed
// to a scratch register that the veneer saves and restores itself.
//
// SAFETY: this is all-or-nothing per code range. If even one x18 site is left un-veneered the
// shadow and the register file disagree and the guest breaks outright, so an encoding we are not
// certain about aborts the whole range (loudly) and leaves it exactly as it is today.
// ===========================================================================================

unsigned long gmem_guest_x18;            // the guest's x18, virtualised into memory

// The four bit positions at which an A64 instruction can carry a 5-bit register operand.
#define XF0   1u                         // bits  4:0   (Rd / Rt)
#define XF5   2u                         // bits  9:5   (Rn)
#define XF10  4u                         // bits 14:10  (Ra / Rt2)
#define XF16  8u                         // bits 20:16  (Rm)
static const int x18_pos[4] = { 0, 5, 10, 16 };

static inline unsigned x18_field(unsigned int w, int pos) { return (w >> pos) & 0x1F; }
static inline unsigned int x18_set_field(unsigned int w, int pos, unsigned r) {
  return (w & ~(0x1Fu << pos)) | ((r & 0x1F) << pos);
}

// Which 5-bit fields of `w` are GENERAL-PURPOSE register operands? Returns a mask of XF*, or -1
// when the encoding is not one we are certain about (the caller must then refuse to touch it).
// Anything that is understood to have no GPR operand at all returns 0.
//
// Exported (not static) so tools/x18check.c can validate every word of every rootfs binary
// against llvm-objdump. Getting this wrong in EITHER direction corrupts the guest, so it is
// checked exhaustively rather than by inspection.
int gmem_x18_gpr_fields(unsigned int w) {
  unsigned op0 = (w >> 25) & 0xF;                    // bits 28:25

  // ---- Architecturally UNALLOCATED : op0 = 0000 (Reserved) and op0 = 0011 -------------------
  // These two top-level classes name no instruction in ANY revision of A64 -- 0000 is "Reserved"
  // (only UDF #imm16 lives there, and UDF has no register operand at all) and 0011 has never been
  // allocated. A word here therefore CANNOT be an instruction that reads or writes x18, whatever
  // its 4:0 / 9:5 / 14:10 / 20:16 fields happen to read as. Falling through to the `return -1`
  // below made the caller assume the worst and report GMEM_X18_UNSUP, which abandoned the WHOLE
  // image to the racy shared global.
  //
  // That is not hypothetical and it is not rare: an aarch64 compiler emits switch JUMP TABLES as
  // runs of 4-byte offsets INSIDE .text, immediately after the `br xN` that dispatches through
  // them. MEASURED on the WebKitGTK rootfs: of the 810 words the classifier could not handle
  // across a whole MiniBrowser run, 786 (97%) are op0 == 0000 and 4 more are op0 == 0011 --
  // values like 00000032, 00000240, 00000244, 00000248 (a table of ascending offsets) and
  // 004e4e4e / 07480644 (byte data). ONE such word, 00000032 inside
  // libjavascriptcoregtk-4.1.so.0.9.11, was enough to drop that entire 24 MB .text onto the racy
  // path, after which `ldr x8,[x18,#32]` at +0x18983ec read x18 == 0 and faulted.
  //
  // This is the opposite of RDX18_PARTIAL. PARTIAL skips a word the classifier says DOES name
  // x18 and rewrites everything around it -- which is how it planted `brk` over libcrypto's K256
  // round constants. This says the word names no register at all, so nothing is skipped that
  // should have been rewritten and nothing is rewritten that should have been skipped.
  // op0 == 0b0001 is included on measurement, not on the architecture text. It is unallocated in
  // ARMv8.0-8.6, but ARMv9.2-A gave it to SME -- so unlike 0000 and 0011 it is not undefined "in
  // every revision", and an SME load/store CAN name a GPR base. It is safe HERE because (a) this
  // rootfs is built for the armv8-a baseline and contains no SME, and (b) tools/x18check.sh diffs
  // this classifier against llvm-objdump, which DOES decode SME -- so a real SME instruction
  // naming x18 would show up as an objdump-only address and fail that gate. Keep it gated behind
  // that check: if x18check ever reports objdump-only sites after a rootfs change, drop the
  // 0x1 here and leave 0x0/0x3.
  if (op0 == 0x0 || op0 == 0x1 || op0 == 0x3) return 0;

  // ---- Loads and Stores : op0 = x1x0 ------------------------------------------------------
  if ((op0 & 0x5) == 0x4) {
    unsigned v = (w >> 26) & 1;                      // SIMD/FP variant: Rt/Rt2 are V registers
    unsigned c = (w >> 27) & 0x7;                    // bits 29:27
    // Load/store MEMORY TAGS (ARMv8.5 MTE): STG/STZG/ST2G/STZ2G/STGM/STZGM/LDGM/LDG.
    //
    //     1101 1001 opc(2) 1 | imm9(9) | op2(2) | Rn(5) | Rt(5)
    //
    // This has to come BEFORE the `c` dispatch below, which files bits 29:27 == 011 under "LDR
    // literal, refuse" -- and refusing is not free. `gmem_x18_classify()` answers a -1 by treating
    // ANY 5-bit field that reads 18 as a register reference, and in `stz2g x0, [x3, #-0x40]`
    // (d9ffc860) the imm9/op2 bits at 14:10 read 0b10010 = 18. That one accident sent the WHOLE of
    // glibc's libc.so.6 to the racy shared gmem_guest_x18 -- memcpy, strlen, malloc, every string
    // and memory primitive in every glibc process -- for four instructions inside <uselib> that
    // never mentioned x18. musl has no MTE code, which is why only glibc ever tripped it.
    //
    // Every member of the class has its general-purpose operands in exactly Rt (4:0) and Rn (9:5);
    // 20:12 is an immediate and 11:10 selects the addressing mode. Disjoint from LDR (literal),
    // whose top byte is 0x18/0x58/0x98 rather than 0xD9. STGP is NOT in this class (bits 29:27 ==
    // 101) and is already handled correctly by the load/store-pair clause below.
    //
    // NOTE for anyone who follows this up: Apple Silicon has no FEAT_MTE, so these instructions
    // would SIGILL if they ever executed. glibc reaches them only when auxv advertises
    // HWCAP2_MTE, so the shim must not set that bit. Classifying them correctly is still required
    // -- the classifier runs over every word of the image whether or not it is ever reached.
    if ((w & 0xFF200000u) == 0xD9200000u) return XF0 | XF5;
    // Advanced SIMD load/store multiple- and single-structure (LD1/ST1/LD2...): the data register
    // Rt is a V register, but the base Rn and the post-index Rm are ordinary GPRs.
    if ((w & 0xBE000000u) == 0x0C000000u) return XF5 | XF16;
    if (c == 0x7) {                                  // load/store register
      unsigned m = (w >> 24) & 0x3;                  // bits 25:24
      if (m == 1) return (v ? 0u : XF0) | XF5;       //   unsigned immediate offset
      if (m == 0) {
        if (((w >> 21) & 1) == 0)                    //   unscaled / post-index / pre-index
          return (v ? 0u : XF0) | XF5;
        if (((w >> 10) & 3) == 2)                    //   register offset: Rm is a GPR index
          return (v ? 0u : XF0) | XF5 | XF16;
        return -1;                                   //   atomics / LDAPR / PAC: not handled
      }
      return -1;
    }
    if (c == 0x5)                                    // load/store register pair
      return (v ? 0u : (XF0 | XF10)) | XF5;
    if (c == 0x3) return -1;                         // LDR (literal) -- PC-relative, special-cased
    // ---- Load/store EXCLUSIVE and ORDERED : bits 29:24 == 001000 ---------------------------
    //
    //     size(2) 001000 o2 L o1 | Rs(5) | o0 | Rt2(5) | Rn(5) | Rt(5)
    //
    // LDXR/STXR/LDAXR/STLXR and their pair forms, LDAR/STLR, LDLAR/STLLR, and the ARMv8.1 LSE
    // CAS/CASP family all live here. EVERY 5-bit field in the class is a general-purpose
    // register -- Rs (20:16), Rt2 (14:10), Rn (9:5), Rt (4:0) -- and the class contains NO
    // immediate field at all, so naming all four is exact, not merely conservative. The fields a
    // given encoding does not use are RES1 (0b11111 = 31), which can never read as 18, so this
    // cannot manufacture a false positive either. Verified against llvm-objdump by
    // tools/x18check.sh, which is the instrument that has to agree.
    //
    // This was `return -1`, and a -1 makes gmem_x18_classify() assume the worst and answer
    // GMEM_X18_UNSUP, which abandons the ENTIRE IMAGE to the racy shared global. Exactly ONE
    // instruction did that to WebKit: `ldar x18, [x18]` (c8dffe52) at +0x287e9b4, inside
    // WebKit::WebProcessMain, sent all 88 MB of libwebkit2gtk-4.1.so.0.19.9 down the racy path.
    if (c == 0x1 && ((w >> 24) & 0x7) == 0) return XF0 | XF5 | XF10 | XF16;
    return -1;                                       // SIMD multi-structure, LDAPR/STLR, ...
  }

  // ---- Data Processing -- Immediate : op0 = 100x ------------------------------------------
  if ((op0 & 0xE) == 0x8) {
    switch ((w >> 23) & 0x7) {                       // bits 25:23
      case 0: case 1: return -1;                     // ADR / ADRP -- PC-relative, special-cased
      case 2: case 3: return XF0 | XF5;              // add/subtract (immediate, with tags)
      case 4:         return XF0 | XF5;              // logical (immediate)
      case 5:         return XF0;                    // move wide (immediate): 20:5 is imm16
      case 6:         return XF0 | XF5;              // bitfield
      case 7:         return XF0 | XF5 | XF16;       // extract (EXTR/ROR: Rm is real)
    }
    return -1;
  }

  // ---- Data Processing -- Register : op0 = x101 -------------------------------------------
  if ((op0 & 0x7) == 0x5) {
    if (((w >> 28) & 1) == 0)                        // logical / add-sub, shifted or extended reg
      return XF0 | XF5 | XF16;
    switch ((w >> 21) & 0xF) {                       // bits 24:21
      case 0x0: return XF0 | XF5 | XF16;             // add/subtract with carry
      case 0x2:                                      // conditional compare: no Rd (it is nzcv)
        return ((w >> 11) & 1) ? XF5                 //   immediate form: 20:16 is imm5
                               : (XF5 | XF16);       //   register form
      case 0x4: return XF0 | XF5 | XF16;             // conditional select
      case 0x6:                                      // data-processing 1-source / 2-source
        return ((w >> 30) & 1) ? (XF0 | XF5)         //   1-source: 20:16 is opcode2
                               : (XF0 | XF5 | XF16); //   2-source: Rm
      case 0x8: case 0x9: case 0xA: case 0xB:
      case 0xC: case 0xD: case 0xE: case 0xF:
        return XF0 | XF5 | XF10 | XF16;              // data-processing 3-source (MADD/MSUB/MUL)
    }
    return -1;
  }

  // ---- Branches, Exception Generating and System : op0 = 101x ------------------------------
  if ((op0 & 0xE) == 0xA) {
    if ((w & 0x7C000000u) == 0x14000000u) return 0;  // B / BL (imm26)
    if ((w & 0x7E000000u) == 0x34000000u) return -1; // CBZ / CBNZ -- PC-relative, special-cased
    if ((w & 0x7E000000u) == 0x36000000u) return -1; // TBZ / TBNZ -- PC-relative, special-cased
    if ((w & 0xFE000000u) == 0x54000000u) return 0;  // B.cond
    if ((w & 0xFF000000u) == 0xD4000000u) return 0;  // SVC/HVC/BRK/... (imm16, no registers)
    if ((w & 0xFFC00000u) == 0xD5000000u) return XF0;// MSR/MRS/hints: Rt at 4:0
    if ((w & 0xFE000000u) == 0xD6000000u) return XF5;// BR/BLR/RET/ERET: Rn at 9:5
    return -1;
  }

  // ---- SIMD / scalar floating point : op0 = x111 -------------------------------------------
  if ((op0 & 0x7) == 0x7) {
    // Nearly everything here names V registers, where "18" means v18 and must NOT be rewritten.
    // Exactly two classes mix in a general-purpose register, and in both the GPR is on one
    // specific side, so they can be decoded rather than refused.
    if ((w & 0x5F000000u) == 0x1E000000u) {          // scalar floating point
      int conv = (((w >> 21) & 1) == 0)              //   float <-> fixed-point
              || (((w >> 10) & 0x3F) == 0);          //   float <-> integer (incl. FMOV general)
      if (!conv) return 0;                           //   compare / 1-source / 2-source / 3-source
      unsigned opcode = (w >> 16) & 0x7;
      // SCVTF/UCVTF and "FMOV from general" read the GPR in Rn; everything else (FCVT*, "FMOV to
      // general", FJCVTZS) writes it in Rd.
      return (opcode == 2 || opcode == 3 || opcode == 7) ? XF5 : XF0;
    }
    if ((w & 0x9FE08400u) == 0x0E000400u) {          // Advanced SIMD copy
      if ((w >> 29) & 1) return 0;                   //   INS (element): V <- V
      switch ((w >> 11) & 0xF) {                     //   imm4
        case 0x0: return 0;                          //   DUP (element): V <- V
        case 0x1: case 0x3: return XF5;              //   DUP (general) / INS (general): V <- GPR
        case 0x5: case 0x7: return XF0;              //   SMOV / UMOV: GPR <- V
      }
      return -1;
    }
    return 0;                                        // everything else here is V-register only
  }

  return -1;                                         // SVE / SME / reserved: never seen, never touched
}

// ---- register branches -----------------------------------------------------------------------
// BR / BLR / RET are the one family the "spill, re-execute, restore" veneer shape below cannot
// service: control LEAVES the veneer at the re-executed instruction, so the epilogue never runs.
// They therefore get their own treatment, and the first thing that needs is an exact decode.
// Only the plain, non-pointer-auth encodings are recognised (op4 == 0, Z == 0, M == 0):
//   0 = BR Xn, 1 = BLR Xn, 2 = RET Xn, -1 = anything else.
static int vn_branch_reg_op(unsigned int w) {
  switch (w & 0xFFFFFC1Fu) {                      // strip Rn (9:5); everything else must match
    case 0xD61F0000u: return 0;                   // BR  Xn
    case 0xD63F0000u: return 1;                   // BLR Xn
    case 0xD65F0000u: return 2;                   // RET Xn
  }
  return -1;
}

// A register branch through x18 that NO veneer can service.
//
// BLR is fine (see vn_build): x16 (IP0) is architecturally dead across a call, so the veneer can
// borrow it, and x30 is about to be written by the call anyway.
//
// BR and RET are not. AAPCS64 only licenses clobbering x16/x17 *between a routine and a
// subroutine it calls* -- i.e. at a call or a return boundary. A `br xN` is not necessarily
// either: in EVERY real occurrence in this rootfs it is an intra-function jump-table dispatch,
// and one of them (JSC 0x97b260, `lsr w17, w15, #13` immediately before `br x18`) has x17 LIVE
// across the branch. There is no register a veneer could borrow, so such an image goes on the
// trap path instead, where the handler performs the branch itself and borrows nothing.
static int insn_unveneerable_x18_branch(unsigned int w) {
  int op = vn_branch_reg_op(w);
  return op >= 0 && op != 1 && ((w >> 5) & 31) == 18;
}

// What kind of x18 site is `w`, and (for the generic kind) which fields hold 18? Exported (see
// gmem.h) so tools/x18check.c can validate exactly the decision the rewriter makes.
int gmem_x18_classify(unsigned int w, unsigned* fields_out) {
  *fields_out = 0;
  // Pointer-authenticated register branches (BRAA/BLRAB/RETAA/...) and anything else in the
  // register-branch space that is not exactly BR/BLR/RET cannot be re-executed out of line at
  // all: the authentication is keyed on the address the instruction executes at, and neither a
  // veneer nor the trap handler can reproduce it. Refuse rather than silently strip the auth.
  if ((w & 0xFE000000u) == 0xD6000000u && ((w >> 5) & 31) == 18 && vn_branch_reg_op(w) < 0)
    return GMEM_X18_UNSUP;
  // A TLS access through x18 cannot be veneered: the svc/tpidr pass turns it into a brk whose
  // immediate no longer names a register, and the handler answers it from the register file.
  // Refuse it in both spellings -- before that pass (raw msr/mrs) and after it (brk).
  if (w == (INSN_MSR_TPIDR | 18) || w == (INSN_MRS_TPIDR | 18)) return GMEM_X18_UNSUP;
  if (w == BRK_IMM(BRK_TLS_WRITE + 18) || w == BRK_IMM(BRK_TLS_READ + 18)) return GMEM_X18_UNSUP;

  if ((w & 0x1F000000u) == 0x10000000u)                                    // ADR / ADRP
    return ((w & 0x1F) == 18) ? GMEM_X18_ADR : GMEM_X18_NONE;
  if ((w & 0x3B000000u) == 0x18000000u) {                                  // LDR (literal)
    if ((w & 0x1F) != 18) return GMEM_X18_NONE;
    if ((w >> 26) & 1) return GMEM_X18_NONE;                                    //   SIMD/FP: v18, not x18
    if ((w >> 30) == 3) return GMEM_X18_NONE;                                   //   PRFM: no register
    return GMEM_X18_LDRLIT;
  }
  if ((w & 0x7E000000u) == 0x34000000u)                                    // CBZ / CBNZ
    return ((w & 0x1F) == 18) ? GMEM_X18_CBZ : GMEM_X18_NONE;
  if ((w & 0x7E000000u) == 0x36000000u)                                    // TBZ / TBNZ
    return ((w & 0x1F) == 18) ? GMEM_X18_TBZ : GMEM_X18_NONE;

  int m = gmem_x18_gpr_fields(w);
  if (m < 0) {
    // Not understood. It only matters if it could actually be naming x18 -- and since we cannot
    // decode it, any field that reads as 18 has to be treated as a real reference.
    for (int i = 0; i < 4; i++) if (x18_field(w, x18_pos[i]) == 18) return GMEM_X18_UNSUP;
    return GMEM_X18_NONE;
  }
  unsigned f = 0;
  for (int i = 0; i < 4; i++)
    if ((m & (1u << i)) && x18_field(w, x18_pos[i]) == 18) f |= (1u << i);
  if (!f) return GMEM_X18_NONE;
  *fields_out = f;
  return GMEM_X18_PLAIN;
}

// ---- veneer arena ------------------------------------------------------------------------
// One JIT region shared by every image. `b` reaches +-128MB, which comfortably covers the few
// tens of MB the kernel spreads our MAP_JIT regions across; a site out of range is reported and
// aborts its range rather than being silently left behind.

#define VN_ARENA  (1UL << 20)
static unsigned char* g_vn_base;
static size_t         g_vn_cap, g_vn_used;

// Reserve room for a WHOLE range's veneers in one arena, so they are contiguous and a single
// reachability test covers them all. Without this, a range with thousands of sites (Mesa's
// software rasterizer has 6001, libLLVM 13735) rolls over VN_ARENA mid-range and the next arena
// is placed by the kernel wherever it likes -- possibly out of `b` range, which used to abort the
// range half-patched. Returns the base, or 0 if a big enough arena cannot be reserved.
// --- P10 EXPERIMENT: plain (non-MAP_JIT) veneer arena placed NEXT TO a runtime-exec region ---
// The shared MAP_JIT arena is kernel-placed and routinely lands >128 MB from the anonymous
// executable regions LLVM's MCJIT publishes, so `b` cannot reach it and every JIT x18 site is
// left raw (-> `str q0,[x18,x11]` with x18 == 0). Runtime-exec regions are PLAIN memory (the
// binary carries allow-unsigned-executable-memory), so their veneers can live in plain memory
// too, mmap'd with a hint right next to the region.
static unsigned long  g_vn_hint;        // != 0 while rewriting a runtime-exec anon region
static unsigned char* g_plain_base;     // arena for the CURRENT x18_neutralize_ranges() call
static size_t         g_plain_len;
static int            g_plain_rwx;

static int b_in_range(unsigned long from, unsigned long to);

// When the range being rewritten sits INSIDE a larger mapping, hints anchored on the range itself
// all land on memory that is already mapped. Without MAP_FIXED the kernel then places the arena
// wherever it likes — routinely outside `b`'s reach — and every site in the range is left raw.
// That is not a corner case: it is the normal shape of a guest JIT, whose pages live inside one
// big pool reservation. JSC's pool is 512 MB, so a page 480 KB into it has 512 MB of mapped
// memory around it and not one free byte within the 192 KB these hints used to probe.
// Anchor the SEARCH on the enclosing region's edges when we know them, while still testing
// REACHABILITY against the actual range — the two are different addresses and conflating them is
// what made the JIT rewrite silently do nothing.
static unsigned long g_vn_region_lo, g_vn_region_hi;

// A pre-reserved arena to carve from, set while rewriting a page of a W^X pool. When present it
// is used INSTEAD of the mmap search below: the reservation was placed next to the pool before
// the address space around it filled up, so it is reachable by construction rather than by luck.
static uintptr_t* g_vn_pre_next;
static uintptr_t  g_vn_pre_hi;

// Claim `cap` bytes of ACTUALLY FREE address space closest to [va, va+va_len), by walking the
// Mach VM map for a hole and taking it with VM_FLAGS_FIXED.
//
// This exists because mmap cannot express what we need. An mmap hint is advisory: when the hinted
// address is occupied the kernel does not look nearby, it falls back to its own arena far away --
// so probing hints is a lottery that a busy address space loses (measured: 1 success in 5 over a
// 64-candidate sweep). MAP_FIXED would place it exactly but silently unmaps whatever is already
// there, which is unusable. mach_vm_allocate with VM_FLAGS_FIXED is the primitive with the right
// semantics: it FAILS with KERN_NO_SPACE rather than clobbering, so a lost race costs one retry.
//
// Holes are walked in ascending order, so searching BELOW keeps the last fit (closest under the
// range) and searching ABOVE takes the first (closest over it). Closest wins, which leaves the
// most `b` margin for the rest of the range.
#define ARENA_WINDOW  (120UL << 20)             // inside `b`'s +/-128 MB, with margin

static unsigned long arena_hole_near(unsigned long va, size_t va_len, size_t cap) {
  unsigned long end   = (unsigned long)PAGE_UP(va + va_len);
  unsigned long wlo   = va > ARENA_WINDOW ? va - ARENA_WINDOW : (unsigned long)GPAGE;
  unsigned long whi   = end + ARENA_WINDOW;
  unsigned long best  = 0, bestd = ~0UL, biggest = 0;
  int nregions = 0;

  // ONE walk across the whole window. mach_vm_region reports the first mapped region at or after
  // the probe address, so the space between the probe and that region is a hole. A gap below the
  // range is best used at its TOP and one above at its BOTTOM, so the arena ends up as close to
  // the range as the map allows -- that closeness is what leaves `b` margin for the rest of it.
  mach_vm_address_t a = (mach_vm_address_t)wlo;
  for (int guard = 0; guard < 8192 && (unsigned long)a + cap <= whi; guard++) {
    mach_vm_address_t r = a; mach_vm_size_t sz = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    kern_return_t kr = mach_vm_region(mach_task_self(), &r, &sz, VM_REGION_BASIC_INFO_64,
                                      (vm_region_info_t)&info, &cnt, &obj);
    unsigned long gap_lo = (unsigned long)a;
    unsigned long gap_hi = (kr == KERN_SUCCESS && (unsigned long)r < whi) ? (unsigned long)r : whi;
    if (kr == KERN_SUCCESS) nregions++;
    if (gap_hi > gap_lo) {
      if (gap_hi - gap_lo > biggest) biggest = gap_hi - gap_lo;
      if (gap_hi - gap_lo >= cap) {
        unsigned long cand = (gap_hi <= va) ? gap_hi - cap : gap_lo;
        unsigned long d = cand < va ? va - cand : cand - va;
        if (d < bestd && b_in_range(va, cand) && b_in_range(end, cand + cap)) {
          bestd = d; best = cand;
        }
      }
    }
    if (kr != KERN_SUCCESS) break;                       // nothing mapped ahead
    mach_vm_address_t next = (mach_vm_address_t)(r + sz);
    if ((unsigned long)next <= gap_lo) break;            // no forward progress: stop
    a = next;
  }
  if (!best) {
    if (g_verbose)
      fprintf(stderr, "[gmem] x18: no hole >= %zu KB within +/-%lu MB of %#lx (%d region(s) "
                      "walked, biggest gap %lu KB)\n",
              cap / 1024, ARENA_WINDOW >> 20, va, nregions, biggest / 1024);
    return 0;
  }
  mach_vm_address_t got = (mach_vm_address_t)best;
  if (mach_vm_allocate(mach_task_self(), &got, cap, VM_FLAGS_FIXED) != KERN_SUCCESS) return 0;
  if ((unsigned long)got != best ||                      // FIXED must honour the address exactly
      !b_in_range(va, (unsigned long)got) || !b_in_range(end, (unsigned long)got + cap)) {
    mach_vm_deallocate(mach_task_self(), got, cap);
    return 0;
  }
  return (unsigned long)got;                    // VM_PROT_DEFAULT is R+W; plain_finish() flips R+X
}

static unsigned long plain_arena_near(unsigned long va, size_t va_len, size_t need) {
  size_t cap = (size_t)PAGE_UP(need < GPAGE ? GPAGE : need);
  // The pool's own reservation is tried first, but a MISS here must fall through to the hole
  // search rather than give up: a pool page past the reservation's `b` reach, or one arriving
  // after the reservation is used up, is exactly the case that used to end as a raw page.
  if (g_vn_pre_next && (unsigned long)*g_vn_pre_next + cap <= g_vn_pre_hi &&
      b_in_range(va, (unsigned long)*g_vn_pre_next) &&
      b_in_range(va + va_len, (unsigned long)*g_vn_pre_next + cap)) {
    unsigned long a = (unsigned long)*g_vn_pre_next;
    *g_vn_pre_next += cap;
    // Carved page-aligned and never handed out twice, so plain_finish() may flip exactly this
    // chunk to R+X without disturbing veneers already executing elsewhere in the reservation.
    g_plain_base = (unsigned char*)a; g_plain_len = cap; g_plain_rwx = 0;
    return a;
  }
  // Ask the kernel where the free space actually is, before falling back to guessing at it. This
  // is what lets a range the guest placed itself -- MAP_FIXED text, or memory it mprotect'ed to
  // executable -- still get veneers: we cannot move the range, but we can find a real hole beside
  // it. Retried a few times because VM_FLAGS_FIXED loses to a concurrent mapping rather than
  // stealing from it, and another thread is free to take the hole between the walk and the claim.
  for (int i = 0; i < 4; i++) {
    unsigned long a = arena_hole_near(va, va_len, cap);
    if (!a) break;
    g_plain_base = (unsigned char*)a; g_plain_len = cap; g_plain_rwx = 0;
    return a;
  }
  unsigned long lo_anchor = g_vn_region_lo ? g_vn_region_lo : va;
  unsigned long after = g_vn_region_hi ? (unsigned long)PAGE_UP(g_vn_region_hi)
                                       : (unsigned long)PAGE_UP(va + va_len);
  for (int i = 0; i < 24; i++) {
    unsigned long hint = (i & 1) ? after + (unsigned long)(i / 2) * cap
                                 : lo_anchor - cap - (unsigned long)(i / 2) * cap;
    void* p = mmap((void*)hint, cap, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_ANON | MAP_PRIVATE, -1, 0);
    int rwx = 1;
    if (p == MAP_FAILED) {
      p = mmap((void*)hint, cap, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
      rwx = 0;
    }
    if (p == MAP_FAILED) continue;
    unsigned long a = (unsigned long)p;
    if (b_in_range(va, a) && b_in_range(va + va_len, a + cap)) {
      g_plain_base = (unsigned char*)p; g_plain_len = cap; g_plain_rwx = rwx;
      return a;
    }
    munmap(p, cap);
  }
  return 0;
}

// Publish veneer words: memcpy into the plain arena, or gmem_map_code into the JIT arena.
static int vn_write(unsigned long dst, const void* src, size_t len) {
  if (g_plain_base && dst >= (unsigned long)g_plain_base &&
      dst + len <= (unsigned long)g_plain_base + g_plain_len) {
    memcpy((void*)dst, src, len);
    return 0;
  }
  return gmem_map_code((void*)dst, src, len);
}

static void plain_finish(void) {
  if (!g_plain_base) return;
  if (!g_plain_rwx && mprotect(g_plain_base, g_plain_len, PROT_READ | PROT_EXEC) != 0)
    fprintf(stderr, "[gmem] x18: plain veneer arena could not be made executable: %s\n",
            strerror(errno));
  sys_icache_invalidate(g_plain_base, g_plain_len);
  g_plain_base = NULL; g_plain_len = 0;
}

static unsigned long vn_reserve_run(size_t words) {
  size_t need = words * 4;
  if (!need) return 0;
  if (g_vn_hint) return 0;                      // caller uses plain_arena_near() instead
  if (!g_vn_base || g_vn_used + need > g_vn_cap) {
    size_t cap = VN_ARENA;
    while (cap < need) cap *= 2;
    unsigned char* p = (unsigned char*)gmem_reserve(cap);
    if (!p) return 0;
    g_vn_base = p; g_vn_cap = cap; g_vn_used = 0;
  }
  return (unsigned long)(g_vn_base + g_vn_used);
}

// Trap STUBS, unlike veneers, are entered by writing the PC in the exception handler
// (`*next = t->stub` in gmem_x18_trap_enter), never by a `b` planted in the image. So they need no
// reachability at all and the kernel-placed shared MAP_JIT arena always serves -- even while
// g_vn_hint is steering VENEERS into a plain arena next to the image. Without this the two
// mechanisms fight: g_vn_hint makes vn_reserve_run() return 0, x18_trap_ranges() reports "no room
// for N trap stubs", and an image that only needed the trap path (a `br x18`, a misaligned SP) is
// abandoned to the racy shared global instead. That combination is not hypothetical: glibc's
// ld.so reserves a DSO PROT_NONE and MAP_FIXEDs its text in, which is the path that now sets
// g_vn_hint, and libcrypto.so.3 is exactly an image whose `br x18` needs the trap path.
static unsigned long vn_reserve_stubs(size_t words) {
  unsigned long saved = g_vn_hint;
  g_vn_hint = 0;
  unsigned long r = vn_reserve_run(words);
  g_vn_hint = saved;
  return r;
}

// --- A64 encoders used by the veneers ---
static inline unsigned int E_STUR_SP(unsigned r, int off) {      // stur xr, [sp, #off]
  return 0xF8000000u | (((unsigned)off & 0x1FF) << 12) | (31u << 5) | (r & 31); }
static inline unsigned int E_LDUR_SP(unsigned r, int off) {      // ldur xr, [sp, #off]
  return 0xF8400000u | (((unsigned)off & 0x1FF) << 12) | (31u << 5) | (r & 31); }
static inline unsigned int E_LDR_LIT(unsigned r, long delta) {   // ldr xr, <pc+delta>
  return 0x58000000u | ((unsigned)((delta / 4) & 0x7FFFF) << 5) | (r & 31); }
static inline unsigned int E_LDR_OFF(unsigned rt, unsigned rn) { // ldr xt, [xn]
  return 0xF9400000u | ((rn & 31) << 5) | (rt & 31); }
static inline unsigned int E_STR_OFF(unsigned rt, unsigned rn) { // str xt, [xn]
  return 0xF9000000u | ((rn & 31) << 5) | (rt & 31); }
// P10: per-thread x18 storage. TPIDRRO_EL0 IS per-thread on Darwin/arm64 (it is what
// _os_tsd_get_base() reads; the low 3 bits are the cpu number and must be masked off), and a
// pthread_key_create()'d key k puts this thread's slot at tsd_base + k*8. MEASURED on this
// machine: three threads read three different bases and their own values.
static inline unsigned int E_MRS_TPIDRRO(unsigned rt) {          // mrs xt, tpidrro_el0
  return 0xD53BD060u | (rt & 31); }
static inline unsigned int E_AND_NOT7(unsigned rd, unsigned rn) {// and xd, xn, #~7
  return 0x927DF000u | ((rn & 31) << 5) | (rd & 31); }
static inline unsigned int E_LDR_IMM(unsigned rt, unsigned rn, unsigned off) {  // ldr xt,[xn,#off]
  return 0xF9400000u | (((off / 8) & 0xFFF) << 10) | ((rn & 31) << 5) | (rt & 31); }
static inline unsigned int E_STR_IMM(unsigned rt, unsigned rn, unsigned off) {  // str xt,[xn,#off]
  return 0xF9000000u | (((off / 8) & 0xFFF) << 10) | ((rn & 31) << 5) | (rt & 31); }

// 0 = veneers use the single global gmem_guest_x18 (racy across threads);
// non-zero = byte offset of this thread's x18 slot from the Darwin TSD base.
static unsigned g_x18_tsd_off;

// ============================================================================================
// THE GUEST'S THREAD POINTER, INLINE — the same TPIDRRO_EL0+TSD trick, applied to tpidr_el0.
//
// `mrs xN, tpidr_el0` is rewritten to `brk #(0x200+N)` and answered by the trap handler. That is
// CORRECT and must stay correct (macOS owns the real TPIDR_EL0 and restores its own value on every
// context switch), but it is not cheap: it is a full trap per THREAD-POINTER READ, and glibc reads
// the thread pointer constantly. MEASURED on the Debian rootfs, `find /usr`: 88-90% of ALL traps
// are this one instruction, ~0.94 s of trap per second of wall clock.
//
// WHY THIS SITE IS THE EASY ONE, and easier than any x18 site. `mrs xN, tpidr_el0` HAS NO SOURCE
// OPERANDS. Its destination register is dead on arrival, so the veneer may simply write it — no
// spill, no borrowed scratch register, no liveness analysis, no SP arithmetic, and nothing to
// restore. Four words, unconditional:
//
//      site:   b <veneer>                  <- 1:1 replacement, clobbers nothing (not even x30)
//      veneer: mrs xN, tpidrro_el0         <- per-thread, and readable at EL0 on Darwin
//              and xN, xN, #~7             <- low 3 bits are the cpu number
//              ldr xN, [xN, #g_tls_tsd_off]
//              b <site+4>
//
// THE ONE HOME INVARIANT APPLIES HERE VERBATIM, and it is the whole risk of this change. The
// veneer reads THIS THREAD's TSD slot; the trap handler must read and write THE SAME WORD, which
// it reaches through gthread::tpidr_slot (recorded by the guest thread itself, exactly as
// gthread::x18_slot is). It matters more here than for x18 because of the traffic ratio P2
// measured: 183,931 reads to 1 WRITE. That single write is the one that INSTALLS the thread
// pointer. If the veneered read and the trapped write disagree about where the home is, every
// read after it returns zero and glibc dereferences it.
//
// The write side is deliberately NOT veneered. `msr tpidr_el0, xN` happens about once per thread,
// so a veneer would buy nothing measurable and would double the amount of new inline code that has
// to be right. It stays on the trap path and writes through the same slot.
static unsigned g_tls_tsd_off;

// THERE IS NO GLOBAL FALLBACK HOME, and that absence is the fix rather than a tidy-up.
//
// This used to be `unsigned long gmem_guest_tpidr;`, returned by gmem_tls_slot() whenever TSD
// storage was off, and gt_bind_x18() recorded it as this thread's home — so every guest thread
// shared one thread pointer. P2 caught it by gating an arm nobody had gated; it also showed up as
// GLib "Resource deadlock avoided" and glibc "unaligned tcache chunk" under gtk3-widget-factory,
// and as a 3/3 zink fault on the GPU path (P3). `fe1f240` defused it at the one call site, with
// `(sl == &gmem_guest_tpidr) ? NULL : sl`.
//
// THAT GUARD WORKS AND IS STILL THE WRONG SHAPE, which is why this goes further: it is a
// pointer-identity test against an implementation detail of this function, written at a call site
// in another file. It protects the caller that exists, not the contract. Any future caller of
// gmem_tls_slot() gets the shared global back, and if this fallback ever changes identity the test
// silently stops matching and the bug returns with NO diagnostic — the same silent-return-of-a-
// fixed-bug shape as the x18 save slot.
//
// So the global is DELETED and the contract moves into the return value: when there is no
// per-thread storage there is NO INLINE HOME, gmem_tls_slot() returns NULL, and the trap handler's
// own per-thread gthread::tpidr is the only home there has ever been. There is now nothing for a
// caller to compare against and nothing to get wrong.

// RDTLSVENEER=0 leaves every TLS site on the trap path: the A/B reference for this whole change.
// RDTLS_MIXN=n (n>=2) puts every n-th site on the trap path and veneers the rest, i.e. mixes the
// two paths inside one image at per-SITE granularity — the same shape as RDX18_MIXN, and for the
// same reason: whole-image granularity cannot detect a disagreement between the veneer's home and
// the handler's home, because within one image every site agrees.
static int g_tls_veneer = 1;
static int g_tls_mixn;

unsigned long* gmem_tls_slot(void) {
  if (!g_tls_tsd_off) return NULL;      // no inline home: the trap handler's storage IS the home
  unsigned long base;
  __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(base));
  return (unsigned long*)((base & ~7UL) + g_tls_tsd_off);
}
void gmem_tls_seed(unsigned long v) { unsigned long* p = gmem_tls_slot(); if (p) *p = v; }

// TEST-ONLY (RDX18_MIXN=n, n >= 2): put every n-th veneerable x18 site on the TRAP path and leave
// the rest veneered, i.e. deliberately MIX the two paths inside one image, at per-SITE
// granularity. Default 0 = off, and nothing below runs.
//
// This exists to make the "both paths, one word" invariant TESTABLE rather than merely argued.
// Whole-image granularity hides a divergence between the veneers' home and the trap handler's
// home, because within any one image every site agrees; mixing is what makes the two homes meet
// inside a single dependency chain. tests/wl/x18mix.c is built around this knob: under
// RDX18_MIXN it reports 0 mismatches only if the two paths really are the same word.
static int g_x18_mixn;

// RDX18_BRWHOLE=1: put the WHOLE image on the trap path when it contains a `br`/`ret x18`, which
// is what this file did before per-site branch trapping. This is the A/B CONTROL for that change,
// and it is the only knob that restores the old policy.
//
// Its reachable FAIL branch is `tools/x18check -paths`: the SAME binary must print TRAP for
// libjavascriptcoregtk with RDX18_BRWHOLE=1 and VENEER without it. If both arms print the same
// verdict, the flag is not wired to anything and no measurement taken with it means anything.
//
// It must be listed in kCarry (src/shim.c) or it is dropped at every execve -- i.e. in exactly
// the WebKit child processes whose x18 cost is the point of the measurement, while the parent
// still prints a plausible "engaged" line.
//
// ONE READER, on purpose. tools/x18check -paths is the instrument this change is judged by, and it
// links this object rather than re-reading the environment, so the tool and the rewriter cannot
// disagree about the policy. The read is lazy because a tool never calls gmem_x18_tsd_init().
// DEFAULT IS WHOLE-IMAGE, i.e. per-site is OPT-IN via RDX18_BRPERSITE=1, and that is deliberate
// even though per-site is the correct design and measures 4-23x on the interpreter.
//
// Per-site veneering of libjavascriptcoregtk BREAKS JSC's JIT, 3/3, reproduced independently:
// jsbench2's own cold-vs-hot oracle fires at the FIRST kernel ("props DISAGREES ... 1410090408 vs
// -1139869225 (JIT miscompile)") and two of three runs take a fault using an EMPTY JSValue as a cell
// pointer. The interpreter arm on the SAME veneered words is spotless over ~50M operations, so no
// veneer KIND is broken -- the defect is specific to what the JIT does and the interpreter does not.
//
// The suspected mechanism, and the reason this is opt-in rather than reverted: a VENEER SPILLS TWO
// REGISTERS 16 BYTES BELOW SP; the TRAP PATH SPILLS NOTHING. Whole-image trapping was MASKING a
// below-SP conflict, and JSC has one by design -- CallFrameShuffler writes the callee's frame below
// SP before moving SP (JSC's own comments call it the danger zone), on every polymorphic/varargs
// call in baseline/DFG/FTL and NEVER in LLInt. That predicts all four observations: JIT-only, early,
// an 8-byte JSValue slot corrupted, interpreter clean. The garbage value differs run to run, which
// fits stack corruption rather than a deterministic miscompile.
//
// So the fix is not here. It is a veneer save area that does not touch the stack, which would make
// veneers safe for ANY image that writes below SP -- and this change moves six other images onto
// veneers too. Flip this default the moment that lands; the pass-0 split below is sound and tested.
int gmem_x18_brwhole(void) {
  static int v = -1;
  if (v < 0) v = getenv("RDX18_BRPERSITE") == 0;   // whole-image unless per-site is asked for
  return v;
}

// RDX18_PARTIAL=1: when a word cannot be CLASSIFIED, skip that word instead of abandoning the
// whole image to the racy shared global.
//
// What the all-or-nothing bail costs, MEASURED (tests/wl/cryptokat.c, published AES/SHA vectors
// against the real rootfs libcrypto.so.3, 20000 iters x 4 threads):
//     thread 1: 34/20000 bad   thread 2: 47/20000   thread 3: 40/20000   thread 4: 43/20000
//     AES-256-GCM ct  got cea7403d4d60116e074ec5d3baf39d18
//                    want cea7403d4d606b6e074ec5d3baf39d18      <- ONE byte, silently
// ~0.2% of AES operations return the wrong answer, no error raised anywhere. SHA-256 is clean,
// which is why spot-checks never caught it. libcrypto lands on that path because ONE embedded
// table word (c2b2e898 at 0xbd064) decodes as an unrewritable x18 encoding.
//
// WHY SKIPPING IS SOUND HERE, and it is a narrower claim than "the word is data":
// the bail already leaves that word using x18-in-the-register. Skipping leaves EXACTLY THE SAME
// word in exactly the same state, and differs only in what happens to the REST of the image. So
// skipping cannot be worse than bailing at the skipped word, and is better everywhere else.
// (This is NOT the "trap the ranges you could process" idea from KNOWN_BUGS.md, which cuts a
// range mid-function and splits a live dependency chain between memory and register storage.
// This cuts at ONE word that no veneer or stub could have serviced under either policy.)
//
// ***MEASURED RESULT: DO NOT MAKE THIS THE DEFAULT. IT IS CATASTROPHICALLY WORSE.***
// It is kept only because the experiment is worth being able to repeat.
//
//     ./shim_cli rootfs /cryptokat 20000 4                 176/80000 wrong  (~0.2%)
//     RDX18_PARTIAL=1 ./shim_cli rootfs /cryptokat 20000 4 WARM-UP FAILED on iteration 0
//         SHA-256(abc) got d23e4f50...  want ba7816bf...   -> 100% wrong
//
// The mechanism, confirmed exactly. Skipping the UNSUP words is fine in itself; the damage is
// that proceeding then lets the rewriter plant `brk` over the rest of the image -- INCLUDING
// EMBEDDED CONSTANT TABLES, because .text holds data and nothing here can tell the difference.
// SHA-256's K256 round-constant table lives at VA 0x21e600 in libcrypto's .text, and SIX of its
// 64 constants classify as x18 sites:
//
//     0x21e610  3956c25b  PLAIN   <- brk planted over a round constant
//     0x21e660  983e5152  LDRLIT  <- brk planted over a round constant
//     0x21e698  81c2c92e  UNSUP   (skipped)
//     0x21e6a4  a81a664b  PLAIN   <- brk planted over a round constant
//     0x21e6d8  5b9cca4f  PLAIN   <- brk planted over a round constant
//     0x21e6fc  c67178f2  UNSUP   (skipped)
//
// Four of SHA-256's constants get overwritten, so every digest is wrong from the first block.
// Note the irony this exposes: the two UNSUP words that trigger today's bail are THEMSELVES
// SHA-256 round constants. The bail is being fired by the very table it is accidentally
// protecting.
//
// THE CONCLUSION THIS FORCES: the bail is not the bug, it is a symptom. Fixing libcrypto needs
// CODE/DATA DISCRIMINATION, and neither of the two sources this project has proposed can supply
// it -- libcrypto is stripped (no .symtab, .dynsym covers exported symbols only), and .eh_frame
// leaves 19 gaps in .text totalling ~260 KB whose `ret`/`bl` density matches known code, i.e.
// OpenSSL's hand-written aarch64 assembly, which is emitted without CFI. Real code discovery
// (recursive descent from FDE/PLT/reloc roots) is the only sound route, and it is a much larger
// job than the two one-liners KNOWN_BUGS.md currently proposes.
static int g_x18_partial;

// THIS THREAD's home for the guest's x18 — the exact slot the veneers address inline
// (mrs xJ,tpidrro_el0 / and xJ,xJ,#~7 / ldr|str xK,[xJ,#g_x18_tsd_off]).
//
// ===== THE INVARIANT: x18 HAS EXACTLY ONE HOME PER GUEST THREAD ============================
// This word is it. Every veneer reaches it inline, and the brk/trap handler reaches THE SAME
// WORD through gthread::x18_slot. Nothing synchronises two homes because there are not two.
//
// Do not add a second copy anywhere. There used to be one — `gthread::x18` — and it was safe
// only because x18 neutralisation was all-or-nothing PER IMAGE, so within any one image every
// site agreed on which copy it meant. That made per-SITE mixing silently wrong, which matters:
// pixman (misaligned SP) and every image containing `br x18` must stay on the trap path forever
// and coexist in one process with veneered images, and keeping WebKit veneered instead of wholly
// trapped needs per-site granularity. A previous violation of exactly this class (a per-SITE
// save slot instead of a per-thread one) produced silent, interleaving-dependent render
// corruption. tests/wl/x18mix.c exercises the mixed case on purpose, via RDX18_MIXN.
//
// MUST be called ON the thread it refers to. TPIDRRO_EL0 is per-thread, so reading it from
// anywhere else names the wrong thread's slot. That is exactly why the brk/trap handler cannot
// use this: it runs on the Mach exception-server thread, not the faulting guest thread — which
// is why gt_for(th) exists at all, and why each guest thread records the ADDRESS of its slot in
// gthread::x18_slot while it is still running on itself. The handler dereferences that address,
// which is safe because the faulting thread is stopped for the duration of the exception.
// ===========================================================================================
// Is x18 stored per-thread (in the Darwin TSD slot the veneers address inline), or in the one
// process-global word? Callable from ANY thread -- unlike gmem_x18_slot(), it does not read
// TPIDRRO_EL0. The trap path needs this to tell "this thread never bound its slot, so I am about
// to use the shared global while its VENEERS use per-thread storage" (a silent split home) apart
// from "there is only one home in this configuration" (correct).
int gmem_x18_tsd_on(void) { return g_x18_tsd_off != 0; }

unsigned long* gmem_x18_slot(void) {
  if (!g_x18_tsd_off) return &gmem_guest_x18;
  unsigned long base;
  __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(base));
  return (unsigned long*)((base & ~7UL) + g_x18_tsd_off);
}

// A freshly created pthread's TSD slots are ZERO-INITIALIZED, and nothing but an emitted veneer
// ever writes this one. Without seeding, every veneer that runs on a new guest thread reads
// x18 == 0 — and a veneered `blr x18` then branches through a null pointer.
void gmem_x18_seed(unsigned long v) { *gmem_x18_slot() = v; }

// Per-thread veneers are the DEFAULT. A veneer reaching a single global is racy the moment the
// guest clones a thread, and the trap path that used to cover that costs a Mach exception
// round-trip per x18 instruction (measured 0.026 FPS under llvmpipe).
// RDX18_NOTSD=1 forces the shared global back, for A/B only. Do not combine it with veneers
// that were already emitted TSD-relative: that combination faults inside the veneer itself
// (str x1,[x2,#key*8] against a garbage TSD base).
void gmem_x18_tsd_init(void) {
  static int done; if (done) return; done = 1;
  {                                        // TEST-ONLY: see g_x18_mixn
    const char* m = getenv("RDX18_MIXN");
    if (m) { int n = atoi(m); if (n >= 2) g_x18_mixn = n; }
  }
  g_x18_partial = getenv("RDX18_PARTIAL") != 0;      // see g_x18_partial
  if (gmem_x18_brwhole())                            // see gmem_x18_brwhole (the A/B control)
    fprintf(stderr, "[gmem] x18: RDX18_BRWHOLE=1 — `br x18` puts the WHOLE image on the trap "
                    "path, as it did before per-site branch trapping (A/B CONTROL)\n");
  // RDX18_NOTSD is BENCHMARK-ONLY and is RACY BY CONSTRUCTION. It used to be a real fallback,
  // because the trap path kept a per-thread copy in gthread::x18 and the clone-time conversion
  // moved every site onto it. Both of those are gone: x18 now has ONE home, and with TSD off
  // gmem_x18_slot() hands every guest thread the SAME global word. Measured on tests/wl/x18thr.c:
  // 631403 mismatches under NOTSD versus 0 on the default path. Do not use it to "work around"
  // anything, and do not read a NOTSD result as evidence about the default path.
  if (getenv("RDX18_NOTSD")) {
    fprintf(stderr, "[gmem] x18: RDX18_NOTSD set — ALL guest threads share one x18 word. This is "
                    "RACY and is for A/B measurement only.\n");
    return;
  }
  pthread_key_t k;
  // These two are FATAL, and that is deliberate. There is no longer a safe degraded mode to fall
  // back to: returning here leaves g_x18_tsd_off == 0, which silently routes every thread to the
  // shared global and corrupts guest registers with no diagnostic at all. A loud abort at startup
  // is strictly better than a guest that renders plausible-looking wrong pixels for an hour.
  if (pthread_key_create(&k, NULL) != 0) {
    fprintf(stderr, "[gmem] x18: FATAL — pthread_key_create failed (%s). Per-thread x18 storage is "
                    "mandatory; there is no safe fallback.\n", strerror(errno));
    abort();
  }
  if ((unsigned)k * 8 > 4095u * 8) {
    fprintf(stderr, "[gmem] x18: FATAL — TSD key %u is out of ldr-imm12 reach (max %u). A veneer "
                    "cannot address this slot; there is no safe fallback.\n",
            (unsigned)k, 4095u);
    abort();
  }
  g_x18_tsd_off = (unsigned)k * 8;
  fprintf(stderr, "[gmem] x18: per-thread veneers via TPIDRRO_EL0 + TSD key %u (off %u)\n",
          (unsigned)k, g_x18_tsd_off);

  // ---- the guest thread pointer's own per-thread slot ---------------------------------------
  // NOT fatal if this fails, and that is the one way it differs from x18: leaving g_tls_tsd_off
  // at 0 suppresses TLS veneering entirely, and every site then takes the trap path it takes
  // today. That is a slow but correct configuration, so a loud abort would be worse than a
  // warning. (x18 has no such option: with TSD off its veneers silently share one word.)
  { const char* v = getenv("RDTLSVENEER"); if (v && *v && strcmp(v, "0") == 0) g_tls_veneer = 0; }
  { const char* m = getenv("RDTLS_MIXN"); if (m) { int n = atoi(m); if (n >= 2) g_tls_mixn = n; } }
  if (!g_tls_veneer) {
    fprintf(stderr, "[gmem] tls: RDTLSVENEER=0 — every mrs tpidr_el0 stays on the trap path "
                    "(A/B reference)\n");
    return;
  }
  pthread_key_t tk;
  if (pthread_key_create(&tk, NULL) != 0 || (unsigned)tk * 8 > 4095u * 8) {
    fprintf(stderr, "[gmem] tls: no usable TSD key for the guest thread pointer; TLS stays on "
                    "the trap path\n");
    return;
  }
  g_tls_tsd_off = (unsigned)tk * 8;
  fprintf(stderr, "[gmem] tls: per-thread veneers via TPIDRRO_EL0 + TSD key %u (off %u)%s\n",
          (unsigned)tk, g_tls_tsd_off,
          g_tls_mixn ? " [RDTLS_MIXN: MIXED trap/veneer, TEST MODE]" : "");
}
static inline unsigned int E_B(long delta) {                     // b <pc+delta>
  return 0x14000000u | ((unsigned)((delta / 4) & 0x3FFFFFF)); }
static inline unsigned int E_BR(unsigned r) {                    // br xr
  return 0xD61F0000u | ((r & 31) << 5); }
static inline unsigned int E_MOVZ(unsigned r, unsigned imm16, unsigned sh) {
  return 0xD2800000u | (sh << 21) | ((imm16 & 0xFFFF) << 5) | (r & 31); }
static inline unsigned int E_MOVK(unsigned r, unsigned imm16, unsigned sh) {
  return 0xF2800000u | (sh << 21) | ((imm16 & 0xFFFF) << 5) | (r & 31); }

static int b_in_range(unsigned long from, unsigned long to) {
  long d = (long)to - (long)from;
  return d >= -(1L << 27) && d < (1L << 27);
}

// Pick a register that the veneer may freely borrow: never 18 (gone), never 31 (sp/zr), and never
// one this instruction names in ANY of the four operand positions (whether or not that position is
// really a register field -- being conservative here costs nothing).
static unsigned vn_scratch(unsigned int w, unsigned avoid) {
  for (unsigned r = 0; r < 29; r++) {
    if (r == 18 || r == avoid) continue;
    int clash = 0;
    for (int i = 0; i < 4; i++) if (x18_field(w, x18_pos[i]) == r) clash = 1;
    if (!clash) return r;
  }
  return 32;                                                  // impossible: only 4 fields exist
}

// Build the veneer for one site. Returns the number of words written into `out`, or 0 on failure.
// K holds the guest's x18 while the instruction runs; J holds &gmem_guest_x18. SP is never moved,
// so any SP-relative operand inside the instruction stays valid; the two saved registers live in
// the 16 bytes below SP, which nothing else on this thread touches (AArch64 has no red zone, but
// it also has nothing that writes below SP behind our back -- the kernel never builds frames on
// the guest stack here).
// --- what an instruction does to, or through, SP -------------------------------------------
// The veneer spills two scratch registers below SP and reloads them after re-executing the
// original instruction. That is only safe if the instruction cannot WRITE those slots and cannot
// MOVE SP underneath them -- and plenty of real code does both: 722 of libgallium's 6001 x18
// sites and 2647 of libLLVM's 13735 name x18 and SP together, e.g.
//
//     stp x18, x19, [sp, #-16]!
//
// which lands exactly on the slots the veneer just wrote AND leaves SP 16 lower, so the epilogue
// reloads from the wrong addresses. That corruption is what produced a misaligned SP and a fault
// in a later, innocent veneer.
//
// So: work out the exact SP-relative byte range the instruction touches and the amount it shifts
// SP, and let the caller place its spill slots outside that range. Returns 1 when the effect is
// known (acc_hi <= acc_lo means "touches no SP-relative memory"), 0 when it cannot be determined
// and the veneer must be refused.
static int sp_effect(unsigned int w, long* acc_lo, long* acc_hi, long* delta) {
  *acc_lo = 0; *acc_hi = 0; *delta = 0;
  unsigned rn = (w >> 5) & 31;

  // ---- load/store pair: STP/LDP, signed offset / pre-index / post-index -------------------
  if ((w & 0x3A000000u) == 0x28000000u) {
    if (rn != 31) return 1;                        // not SP-based: cannot touch our slots
    unsigned mode = (w >> 23) & 7;                 // 001 post, 010 signed, 011 pre
    unsigned opc  = w >> 30;
    if (opc == 3) return 0;                        // reserved
    long scale = (opc == 0) ? 4 : 8;               // 32-bit vs 64-bit pair (SIMD never names x18)
    long imm7 = (long)((w >> 15) & 0x7F);
    if (imm7 & 0x40) imm7 -= 128;                  // sign-extend
    long off = imm7 * scale, size = 2 * scale;
    if (mode == 2)      { *acc_lo = off; *acc_hi = off + size; }             // signed offset
    else if (mode == 3) { *acc_lo = off; *acc_hi = off + size; *delta = off; } // pre-index
    else if (mode == 1) { *acc_lo = 0;   *acc_hi = size;       *delta = off; } // post-index
    else return 0;
    return 1;
  }

  // ---- load/store register, unsigned immediate --------------------------------------------
  if ((w & 0x3B000000u) == 0x39000000u) {
    if (rn != 31) return 1;
    unsigned size = w >> 30;
    long off = (long)((w >> 10) & 0xFFF) << size;  // always positive: cannot reach below SP
    *acc_lo = off; *acc_hi = off + (1L << size);
    return 1;
  }

  // ---- load/store register: unscaled, pre-index, post-index, or register offset ------------
  if ((w & 0x3B200000u) == 0x38000000u) {
    if (rn != 31) return 1;
    unsigned size = w >> 30;
    unsigned form = (w >> 10) & 3;                 // 00 unscaled, 01 post, 11 pre, 10 unpriv
    long imm9 = (long)((w >> 12) & 0x1FF);
    if (imm9 & 0x100) imm9 -= 512;                 // sign-extend
    long sz = 1L << size;
    if (form == 0)      { *acc_lo = imm9; *acc_hi = imm9 + sz; }                 // LDUR/STUR
    else if (form == 3) { *acc_lo = imm9; *acc_hi = imm9 + sz; *delta = imm9; }  // pre-index
    else if (form == 1) { *acc_lo = 0;    *acc_hi = sz;        *delta = imm9; }  // post-index
    else return 0;                                 // unprivileged: refuse
    return 1;
  }
  if ((w & 0x3B200C00u) == 0x38200800u) {          // register offset: displacement unknown
    if (rn != 31) return 1;
    return 0;
  }

  // ---- add/sub immediate: the one data-processing form where 31 really means SP ------------
  // Only for the NON-flag-setting variants. In ADDS/SUBS (i.e. cmn/cmp) register 31 is XZR, and
  // treating those as SP writes refuses `cmp x18, xN` -- which is everywhere, and refusing it
  // drops whole images back onto the racy path.
  if ((w & 0x1F000000u) == 0x11000000u) {
    if (w & 0x20000000u) return 1;                 // S bit set: Rd 31 is XZR, SP untouched
    if ((w & 31) != 31) return 1;                  // does not write SP
    long imm = (long)((w >> 10) & 0xFFF);
    if ((w >> 22) & 1) imm <<= 12;                 // shift-by-12 variant
    *delta = (w & 0x40000000u) ? -imm : imm;       // bit30 = SUB
    return 1;
  }
  // ADD/SUB (EXTENDED register) is the only other form where Rd == 31 means SP, and there the
  // amount lives in a register we cannot read at rewrite time. The shifted-register form encodes
  // XZR instead, so it is left alone.
  if ((w & 0x1FE00000u) == 0x0B200000u && !(w & 0x20000000u) && (w & 31) == 31) return 0;

  // Everything else: register 31 encodes XZR, not SP, so SP is untouched.
  return 1;
}

// Pick two 8-byte spill slots below SP that the instruction cannot clobber, and that are still
// reachable (STUR/LDUR imm9 is -256..255) both before and after any SP writeback.
static int vn_pick_slots(unsigned int w, int* s1, int* s2) {
  long alo, ahi, delta;
  if (!sp_effect(w, &alo, &ahi, &delta)) return 0;
  for (long a = -8; a >= -240; a -= 8) {
    long b = a - 8;
    int hits = (ahi > alo) && ((a < ahi && a + 8 > alo) || (b < ahi && b + 8 > alo));
    if (hits) continue;
    if (a - delta < -256 || a - delta > 255) continue;      // epilogue must still encode
    if (b - delta < -256 || b - delta > 255) continue;
    *s1 = (int)a; *s2 = (int)b;
    return 1;
  }
  return 0;
}

static int vn_build(unsigned int* out, int cap, unsigned long vn, unsigned long site,
                    unsigned int w, int kind, unsigned fields) {
  int n = 0;
  #define EMIT(x)  do { if (n >= cap) return 0; out[n++] = (unsigned int)(x); } while (0)
  // `b <guest address>`: the displacement depends on where in the veneer we are, so the
  // word must be built BEFORE n is bumped (out[n++] = f(n) is unsequenced).
  #define EMITB(t) do { unsigned int _b = E_B((long)(t) - (long)(vn + (unsigned long)n * 4)); \
                        EMIT(_b); } while (0)

  // ---- BR / BLR / RET through x18 ---------------------------------------------------------
  // The generic shape below is "spill K and J below SP, load the x18 shadow into K, re-execute
  // the instruction with K substituted, store K back, reload K and J, branch home". Applied to a
  // register branch that is nonsense: `blr x18` becomes `blr K`, so the CALLEE runs -- clobbering
  // the caller-saved K and J and the spill slots beneath SP -- and then returns into the middle
  // of the veneer, which stores through whatever the callee left in J. (`br`/`ret` are worse and
  // silent: control leaves for good and the borrowed registers are simply never restored.)
  //
  // So build a purpose-made veneer with no epilogue at all. For BLR the AAPCS64 intra-procedure
  // scratch register x16 (IP0) is exactly the right tool -- it is what a linker's own long-branch
  // veneer clobbers, so no caller may expect it to survive a call -- and x30 is written by the
  // BLR itself. MEASURED against the real sites: every `blr x18` in libgallium_dri and
  // libLLVM-17 spills x16/x17 to the stack before the call and redefines them after.
  //
  //     mrs x16, tpidrro_el0 ; and x16,x16,#~7 ; ldr x16,[x16,#tsd]   ; x16 = guest x18
  //     movz/movk x30, #<site+4>                                      ; BLR links (absolute:
  //                                                                   ;   adr's +-1MB does not
  //                                                                   ;   cover a 13MB .text)
  //     br  x16                                                       ; NOT blr: LR is already set
  //
  // Nothing is spilled, nothing is restored, and nothing executes after the branch. The x18
  // shadow is deliberately NOT written back: x18 is caller-saved, so its value is dead across the
  // call, and leaving whatever the callee stored there is more faithful than restoring a stale
  // one. This matches gmem_x18_trap_enter()'s handling of the same instruction exactly.
  if (vn_branch_reg_op(w) >= 0 && ((w >> 5) & 31) == 18) {
    if (vn_branch_reg_op(w) != 1) return 0;       // BR / RET: see insn_unveneerable_x18_branch
    int bptr_slot = -1;
    if (g_x18_tsd_off) {
      EMIT(E_MRS_TPIDRRO(16));
      EMIT(E_AND_NOT7(16, 16));
      EMIT(E_LDR_IMM(16, 16, g_x18_tsd_off));
    } else {
      bptr_slot = n; EMIT(0);                     // ldr x16, .Lptr  (patched below)
      EMIT(E_LDR_OFF(16, 16));
    }
    unsigned long lr = site + 4;
    EMIT(E_MOVZ(30, (unsigned)(lr >>  0) & 0xFFFF, 0));
    EMIT(E_MOVK(30, (unsigned)(lr >> 16) & 0xFFFF, 1));
    EMIT(E_MOVK(30, (unsigned)(lr >> 32) & 0xFFFF, 2));
    EMIT(E_MOVK(30, (unsigned)(lr >> 48) & 0xFFFF, 3));
    EMIT(E_BR(16));
    if (!g_x18_tsd_off) {
      if (n & 1) EMIT(0xD503201Fu);               // nop: 8-byte align the literal
      unsigned long lit = vn + (unsigned long)n * 4;
      out[bptr_slot] = E_LDR_LIT(16, (long)lit - (long)(vn + (unsigned long)bptr_slot * 4));
      unsigned long pv = (unsigned long)&gmem_guest_x18;
      EMIT((unsigned int)(pv & 0xFFFFFFFFu));
      EMIT((unsigned int)(pv >> 32));
    }
    return n;
  }

  unsigned K = vn_scratch(w, 32), J = vn_scratch(w, K);
  if (K > 31 || J > 31) return 0;
  int S1, S2;
  if (!vn_pick_slots(w, &S1, &S2)) return 0;      // no slot this instruction cannot reach
  long spdelta = 0; { long lo, hi; sp_effect(w, &lo, &hi, &spdelta); }
  int R1 = S1 - (int)spdelta, R2 = S2 - (int)spdelta;   // SP may have moved by the epilogue

  EMIT(E_STUR_SP(K, S1));
  EMIT(E_STUR_SP(J, S2));
  int ptr_slot = -1, taken_fix = -1;
  if (g_x18_tsd_off) {                                        // xJ = &this thread's x18 slot
    EMIT(E_MRS_TPIDRRO(J));
    EMIT(E_AND_NOT7(J, J));
  } else {
    ptr_slot = n; EMIT(0);                                    // ldr xJ, .Lptr  (patched below)
  }
  #define X18_LD(k_, j_) (g_x18_tsd_off ? E_LDR_IMM((k_), (j_), g_x18_tsd_off) \
                                        : E_LDR_OFF((k_), (j_)))
  #define X18_ST(k_, j_) (g_x18_tsd_off ? E_STR_IMM((k_), (j_), g_x18_tsd_off) \
                                        : E_STR_OFF((k_), (j_)))

  if (kind == GMEM_X18_PLAIN) {
    EMIT(X18_LD(K, J));                                    // xK = guest x18
    unsigned int w2 = w;                                      // rename x18 -> K in every 18-field
    for (int i = 0; i < 4; i++)
      if (fields & (1u << i)) w2 = x18_set_field(w2, x18_pos[i], K);
    EMIT(w2);
    EMIT(X18_ST(K, J));                                    // guest x18 = xK
  } else if (kind == GMEM_X18_ADR) {
    unsigned long imm = (((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3);
    long simm = (long)(imm << 43) >> 43;                      // sign-extend 21 bits
    unsigned long val = (w & 0x80000000u)
        ? ((site & ~0xFFFUL) + ((unsigned long)simm << 12))   // ADRP
        : (site + (unsigned long)simm);                       // ADR
    EMIT(E_MOVZ(K, (unsigned)(val >>  0) & 0xFFFF, 0));
    EMIT(E_MOVK(K, (unsigned)(val >> 16) & 0xFFFF, 1));
    EMIT(E_MOVK(K, (unsigned)(val >> 32) & 0xFFFF, 2));
    EMIT(E_MOVK(K, (unsigned)(val >> 48) & 0xFFFF, 3));
    EMIT(X18_ST(K, J));
  } else if (kind == GMEM_X18_LDRLIT) {
    long simm = ((long)((w >> 5) & 0x7FFFF) << 45) >> 45;     // sign-extend imm19
    unsigned long val = site + (unsigned long)(simm * 4);
    EMIT(E_MOVZ(K, (unsigned)(val >>  0) & 0xFFFF, 0));
    EMIT(E_MOVK(K, (unsigned)(val >> 16) & 0xFFFF, 1));
    EMIT(E_MOVK(K, (unsigned)(val >> 32) & 0xFFFF, 2));
    EMIT(E_MOVK(K, (unsigned)(val >> 48) & 0xFFFF, 3));
    unsigned opc = w >> 30;
    if (opc == 0)      EMIT(0xB9400000u | ((K & 31) << 5) | (K & 31));   // ldr  wK,[xK]
    else if (opc == 1) EMIT(0xF9400000u | ((K & 31) << 5) | (K & 31));   // ldr  xK,[xK]
    else               EMIT(0xB9800000u | ((K & 31) << 5) | (K & 31));   // ldrsw xK,[xK]
    EMIT(X18_ST(K, J));
  } else {                                                    // GMEM_X18_TBZ / GMEM_X18_CBZ
    // Both outcomes must leave the veneer through their own epilogue, because they return to two
    // different guest addresses: the fallthrough to site+4 and the taken edge to the original
    // target. The test itself stays in the instruction, just renamed onto K and re-aimed at the
    // local ".Ltaken" (a few words away, always in range for imm14/imm19).
    unsigned long tgt;
    if (kind == GMEM_X18_TBZ) {
      long off = ((long)((w >> 5) & 0x3FFF) << 50) >> 50;     // sign-extend imm14
      tgt = site + (unsigned long)(off * 4);
    } else {
      long off = ((long)((w >> 5) & 0x7FFFF) << 45) >> 45;    // sign-extend imm19
      tgt = site + (unsigned long)(off * 4);
    }
    if (!b_in_range(vn + 4 * 12, site + 4) || !b_in_range(vn + 4 * 12, tgt)) return 0;
    EMIT(X18_LD(K, J));
    taken_fix = n; EMIT(0);                                   // <test on K> -> .Ltaken
    EMIT(E_LDUR_SP(J, R2));                                   // --- not taken ---
    EMIT(E_LDUR_SP(K, R1));
    EMITB(site + 4);
    long rel = (long)(n - taken_fix) * 4;                     // .Ltaken is here
    out[taken_fix] = (kind == GMEM_X18_TBZ)
        ? ((w & 0xFFF80000u) | (((unsigned)(rel / 4) & 0x3FFF) << 5) | (K & 31))
        : ((w & 0xFF000000u) | (((unsigned)(rel / 4) & 0x7FFFF) << 5) | (K & 31));
    EMIT(E_LDUR_SP(J, R2));                                   // --- taken ---
    EMIT(E_LDUR_SP(K, R1));
    EMITB(tgt);
    goto literal;
  }

  EMIT(E_LDUR_SP(J, R2));
  EMIT(E_LDUR_SP(K, R1));
  if (!b_in_range(vn + (unsigned long)n * 4, site + 4)) return 0;
  EMITB(site + 4);

literal:
  if (!g_x18_tsd_off) {
    if (n & 1) EMIT(0xD503201Fu);                             // nop: 8-byte align the literal
    unsigned long lit = vn + (unsigned long)n * 4;
    out[ptr_slot] = E_LDR_LIT(J, (long)lit - (long)(vn + (unsigned long)ptr_slot * 4));
    unsigned long pv = (unsigned long)&gmem_guest_x18;
    EMIT((unsigned int)(pv & 0xFFFFFFFFu));
    EMIT((unsigned int)(pv >> 32));
  }
  #undef EMIT
  #undef EMITB
  #undef X18_LD
  #undef X18_ST
  return n;
}

// --- WHY THE VENEER CANNOT JUST BORROW THE HARDWARE x18 ---------------------------------------
// The tempting escape from the SP-spill problem below: after rewriting, NO guest instruction
// names the hardware x18 any more, so it looks like a free scratch register at every site --
// which would give a spill-free veneer for EVERY kind of site, not just the ones with a dead
// destination, and would unblock pixman and the `br x18` images in one move.
//
// It does not work, and the reason is worth writing down so nobody re-derives it. MEASURED
// (bench/x18hw.c, which sets x18 to a magic value and re-reads it):
//
//     plain user-mode loop      1000000 checks         8 clobbered
//     across a syscall            10000 checks         0 clobbered
//     across preemption        20000000 checks        76 clobbered
//     across a brk -> signal      20000 checks     20000 clobbered
//     across a brk -> Mach exc    20000 checks     20000 clobbered
//
// The kernel ZEROES x18 on every exception return, and a context switch is an exception return --
// so the value survives an ordinary syscall but not preemption, and never survives a trap. The
// rare clobbers in the "plain" and "preempt" rows are exactly the preemptions. This is the same
// fact that forces x18 to be virtualized in the first place; it applies to the veneer's own
// scratch just as much as to the guest's value.
//
// Replace every x18-naming instruction with a branch to a veneer that keeps x18's value in
// memory instead of the register file. ALL OR NOTHING, and the unit is the whole RANGE LIST, not
// one range: if .text were veneered while .plt were not, x18 would live in memory on one side and
// in the register on the other, and the two would disagree the first time a value crossed between
// them. Callers therefore pass every executable section of an image in a single call.
struct x18_range { unsigned char* buf; size_t n; unsigned long va; };

static inline unsigned int xr_get(const struct x18_range* r, size_t i) {
  unsigned int w; memcpy(&w, r->buf + i * 4, 4); return w;
}
static inline void xr_put(const struct x18_range* r, size_t i, unsigned int w) {
  memcpy(r->buf + i * 4, &w, 4);
}

// ---- which words of an executable section are CODE? `.eh_frame` is the oracle ----------------
//
// SHF_EXECINSTR is not the same question as "is this word an instruction". An aarch64 `.text`
// routinely contains CONSTANT DATA, because that is how perlasm emits it. MEASURED in this rootfs:
// libcrypto.so.3's `.text` holds `ecp_nistz256_precomputed` (160 004 bytes at 0x18273c), the AES
// tables (14 788 bytes at 0xd298c), and ten CRYPTOGAMS banner strings -- 0x59524320 is the ASCII
// " CRY", 0x38764d52 is "RMv8", from "...for ARMv8, CRYPTOGAMS by <appro@openssl.org>".
//
// Every x18 accident this project has had comes from reading those bytes as instructions. 849 words
// of libcrypto's data blobs have a 5-bit field that reads as 18; ONE of them (0x24c6cb00, an
// architecturally UNALLOCATED encoding -- llvm-mc rejects it under `-mattr=+all`) answered
// GMEM_X18_UNSUP and abandoned all 3103 x18 sites in the image to the racy shared global. The
// exact same mechanism previously ate all of glibc and all of libwebkit2gtk.
//
// The primitive for this already exists and is already in the file: `.eh_frame` carries an FDE per
// real function and NONE for a constant table. It is not a heuristic and not a disassembler.
// MEASURED separation over the three images that fall back today:
//
//     libcrypto  FDE-covered code: 0 unclassifiable   outside coverage: 849
//     libgcrypt  FDE-covered code: 0                  outside coverage: 6
//     libgnutls  FDE-covered code: 0                  outside coverage: 3
//
// i.e. ZERO unclassifiable words inside real code, in every image that falls back. Cross-checked
// independently: FDE-covered regions measure 0.0% architecturally-invalid encodings, the uncovered
// blobs measure 19-69%.
//
// This gate is what makes per-site fallback SAFE, and it must land first. Per-site fallback skips
// the word it cannot classify and rewrites everything AROUND it -- and around it, in libcrypto,
// are 1113 table words that classify as perfectly good x18 sites. Enabling per-site fallback
// without this gate plants 1113 branches into OpenSSL's AES and P-256 constants. That is not a
// prediction; it is the recorded RDX18_PARTIAL incident ("planted brk over libcrypto's K256 round
// constants"), measured.
//
// g_code_n == 0 means "no map for this image" and every word is treated as code -- exactly today's
// behaviour, so an image without `.eh_frame` is never made worse.
#define X18_CODE_MAX 32768
static struct { unsigned long lo, hi; } g_code[X18_CODE_MAX];
static int g_code_n;

static unsigned long g_code_skipped_sites;        // see pass 1 of x18_neutralize_ranges
static void x18_code_map_clear(void) { g_code_n = 0; }

static int code_ext_cmp(const void* a, const void* b) {
  unsigned long x = ((const struct { unsigned long lo, hi; }*)a)->lo;
  unsigned long y = ((const struct { unsigned long lo, hi; }*)b)->lo;
  return x < y ? -1 : x > y ? 1 : 0;
}

static int x18_is_code(unsigned long va) {
  if (!g_code_n) return 1;
  int lo = 0, hi = g_code_n - 1;
  while (lo <= hi) {
    int m = lo + (hi - lo) / 2;
    if (va < g_code[m].lo) hi = m - 1;
    else if (va >= g_code[m].hi) lo = m + 1;
    else return 1;
  }
  return 0;
}

// Total veneer BYTES the given ranges need, or 0 if any site cannot be rewritten (in which case
// the caller must leave the image alone entirely). A veneer's LENGTH depends only on
// (w, kind, fields) -- every branch it contains is one word whatever the displacement, and the
// literal alignment pad depends only on the word count -- so this is exact even before the image
// has an address. Probing with `vn == site` makes vn_build's internal range checks trivially pass;
// reachability is judged separately, once, against the real arena.
// Does this instruction leave SP not 16-byte aligned? `sub sp, sp, #imm` / `add sp, sp, #imm`
// with an immediate that is not a multiple of 16. Hand-written assembly does this: pixman's
// aarch64 NEON prologue is
//
//     sub sp, sp, #0xe8        ; 232, and 232 % 16 == 8
//     sub x29, x29, #0x40
//     stp x18, x19, [x29, #-0xa0]
//
// which is legal on Linux because it then addresses everything through x29 and never uses SP as
// a memory base while it is misaligned. A veneer, though, spills through SP -- and AArch64 faults
// on ANY SP-based access when SP is misaligned, whatever the offset. So an image that can run
// with a misaligned SP cannot be veneered at all.
//
// Refusing such an image is safe at this granularity: x18 is a caller-saved temporary in AAPCS64,
// so no value in it survives a call, and an unveneered image therefore cannot disagree with a
// veneered one across a boundary. (Within one image it certainly could -- hence all-or-nothing.)
static int insn_misaligns_sp(unsigned int w) {
  unsigned base = w & 0xFF800000u;                  // 64-bit ADD/SUB immediate, shift == 0
  if (base != 0xD1000000u && base != 0x91000000u) return 0;
  if (((w >> 5) & 31) != 31 || (w & 31) != 31) return 0;    // must be  sp, sp, #imm
  if (w & 0x00400000u) return 0;      // shift-by-12 variant: imm << 12 is always 16-aligned
  return (((w >> 10) & 0xFFF) & 15) != 0;
}

// Which x18 treatment does this ONE word force on its WHOLE image? Pass 0 of
// x18_neutralize_ranges() sends an image to the trap path the moment it sees such a word, so a
// static tool cannot predict veneer-vs-trap-vs-racy without asking the same two predicates the
// real pass asks. Returns 0 = nothing forced, 1 = can run with a misaligned SP, 2 = a register
// branch through x18 no veneer can service. Exposed for tools/x18check.c ONLY.
int gmem_x18_pass0(unsigned int w) {
  if (insn_misaligns_sp(w)) return 1;
  if (insn_unveneerable_x18_branch(w)) return 2;
  return 0;
}

static size_t x18_measure_ranges(const struct x18_range* rs, int nr) {
  unsigned int vbuf[32];
  size_t total = 0;
  for (int r = 0; r < nr; r++) {
    size_t nw = rs[r].n / 4;
    for (size_t i = 0; i < nw; i++) {
      unsigned f;
      if (!x18_is_code(rs[r].va + i * 4)) continue;      // constant data inside .text
      unsigned int w = xr_get(&rs[r], i);
      int k = gmem_x18_classify(w, &f);
      if (k == GMEM_X18_UNSUP) {
        if (g_x18_partial || g_code_n) continue;  // per-site fallback: reported once in pass 1
        fprintf(stderr, "[gmem] x18: %#lx has an x18 operand in an encoding I cannot rewrite "
                        "(%08x); leaving this image on the old (racy) path\n",
                rs[r].va + i * 4, w);
        return 0;
      }
      if (k == GMEM_X18_NONE) continue;
      // `br`/`ret x18` gets a per-site TRAP, not a veneer (see pass 0 of x18_neutralize_ranges),
      // so it needs no arena bytes. Skipping it here is what lets an image containing one still be
      // MEASURED: vn_build refuses register branches, and without this the whole measurement
      // returned 0, mapped_veneer_bytes() reserved no arena next to the image, and the image fell
      // back to a hinted mmap that macOS may refuse -- i.e. straight to the racy global.
      if (!gmem_x18_brwhole() && insn_unveneerable_x18_branch(w)) continue;
      unsigned long site = rs[r].va + i * 4;
      int words = vn_build(vbuf, (int)(sizeof vbuf / 4), site, site, w, k, f);
      if (!words) {
        // A site the veneer emitter cannot build is ONE trap site, not a reason to abandon the
        // image. It costs no arena bytes, so it is simply not counted here.
        //
        // This mattered: Blender 5 uses x18 WITH SP AS THE BASE REGISTER (ldr w18,[sp,x8],
        // ldr q30,[sp,x18], ldrb w18,[sp,x7]). The veneer spills scratch through SP so it cannot
        // re-execute those out of line -- the TRAP path can, because the handler reads the real SP
        // from the exception state and never spills. Before this, three such words put 522 sites of
        // one image on the racy global, and a raw WRITE to x18 there desynchronises every veneered
        // read after it.
        if (g_x18_partial || g_code_n) continue;
        fprintf(stderr, "[gmem] x18: cannot build a veneer for %08x at %#lx; leaving this image "
                        "on the old (racy) path\n", w, site);
        return 0;
      }
      total += (size_t)words * 4;
    }
  }
  return total;
}

// Veneer -> original-instruction map. A fault inside a veneer reports a PC in the JIT arena,
// which belongs to no image and so cannot be attributed by hand; this makes such faults legible.
struct vn_rec { unsigned long lo, hi, site; unsigned int insn; };
#define VN_REC_MAX 65536
static struct vn_rec g_vnrec[VN_REC_MAX];
static int           g_vnrec_n;

static void vn_record(unsigned long lo, unsigned long hi, unsigned long site, unsigned int insn) {
  if (g_vnrec_n >= VN_REC_MAX) return;
  g_vnrec[g_vnrec_n].lo = lo; g_vnrec[g_vnrec_n].hi = hi;
  g_vnrec[g_vnrec_n].site = site; g_vnrec[g_vnrec_n].insn = insn;
  g_vnrec_n++;
}

// The REVERSE of gmem_veneer_lookup: is this GUEST SITE veneered? Needed to name the storage
// regime of one guest word, which is the question a mixed-mode boundary turns on -- a site with
// neither a veneer record nor a trap record, whose word still names x18, is RACY: it executed
// natively into the real x18 and macOS discarded the write.
static int veneered_site(unsigned long site, unsigned int* insn) {
  for (int i = 0; i < g_vnrec_n; i++)
    if (g_vnrec[i].site == site) { if (insn) *insn = g_vnrec[i].insn; return 1; }
  return 0;
}

int gmem_veneer_lookup(unsigned long pc, unsigned long* site, unsigned int* insn) {
  for (int i = 0; i < g_vnrec_n; i++)
    if (pc >= g_vnrec[i].lo && pc < g_vnrec[i].hi) {
      if (site) *site = g_vnrec[i].site;
      if (insn) *insn = g_vnrec[i].insn;
      return 1;
    }
  return 0;
}


// --- trap mode ------------------------------------------------------------------------------
// For images that can run with a misaligned SP, every x18 instruction becomes `brk #0x300`.
// PLAIN instructions are then re-executed out of line from a two-word stub
//     <instruction, x18 renamed to K> ; brk #0x301
// with K loaded from gmem_guest_x18 by the enter handler and written back by the leave handler.
// The PC-relative kinds never reach a stub: their value depends on the address they execute at,
// so they are computed directly in the handler, exactly as vn_build computes them for a veneer.
struct x18_trap {
  unsigned long long hits;       // RDTRAPSTAT: how often this SITE actually fires. Static site
                                 // counts say nothing about cost -- 563 pixman sites are not 563
                                 // equal costs, and only the ones in the inner compositing loop
                                 // matter. This is what decides whether partial veneering pays.
  unsigned long site, stub;
  unsigned int  insn;
  unsigned char kind, K;
  unsigned      fields;
  // NO `saved` here. It used to live in this per-SITE record, which meant every guest thread
  // shared one slot: thread B entering the same site between A's enter and A's leave overwrote
  // A's stash, and A's leave then restored B's value into A's live register K. The enter->leave
  // window is a full Mach exception round-trip, so the collision window is enormous. It caused
  // silent, interleaving-dependent render corruption that needed >=2 concurrent workers and grew
  // with worker count. The save slot is now per-THREAD (caller-supplied); PLAIN sites cannot
  // nest -- enter, one stub instruction, leave -- so one slot per thread is sufficient.              // K's guest value while the stub runs
};
// W2: WebKitGTK needs far more than 32768. libjavascriptcoregtk-4.1 alone asks for ~28005 sites
// on top of the ~18670 the threading conversion already installs, so the table overflowed, the
// whole image fell back to the racy shared global, and x18 read 0 -> SIGSEGV at
// libjavascriptcoregtk+0x18983ec (`ldr x8,[x18,#32]`). Raise it; the array is BSS.
#define X18_TRAP_MAX 262144
static struct x18_trap g_trap[X18_TRAP_MAX];
static int             g_trap_n;

// ---- trap lookup index -------------------------------------------------------------------
// This was a linear scan over g_trap_n, and g_trap_n is ~18670 once threading converts the
// veneers. trap_by() runs TWICE per trapped x18 instruction (keyed on site for enter, on stub
// for leave), i.e. ~37000 comparisons per instruction on top of the Mach round-trip.
//
// Open-addressed, power-of-two, storing index+1 so 0 means empty. Two tables because the two
// lookups use different keys off the same record.
//
// LOCKING: none on the read path, deliberately. Entries are only ever appended while the
// rewriter holds gm_lock, and a site cannot trap until its code has been published as
// executable -- so every insert happens-before any lookup that could find it. The index is
// grown ADDITIVELY rather than rebuilt, so a concurrent reader never sees a half-cleared table.
// W2: this MUST stay >= 2x X18_TRAP_MAX. It is an open-addressed table whose probe STOPS at the
// first empty slot, so once it fills, trap_ix_put() silently drops entries and trap_by() walks
// the whole table and returns NULL -- the BRK then hits no trap record and the process aborts
// with no diagnostic at all. Raising X18_TRAP_MAX without raising this is exactly that failure.
#define TRAP_IX_BITS 20                                   // 1048576 slots for <=262144 entries
#define TRAP_IX_SIZE (1u << TRAP_IX_BITS)
static int g_ix_site[TRAP_IX_SIZE], g_ix_stub[TRAP_IX_SIZE];
static int g_ix_n;                                        // entries already indexed

static inline unsigned trap_hash(unsigned long pc) {       // pc is 4-byte aligned
  return (unsigned)(((pc >> 2) * 0x9E3779B97F4A7C15UL) >> (64 - TRAP_IX_BITS));
}
static void trap_ix_put(int* tab, unsigned long pc, int idx) {
  if (!pc) return;                                        // stub == 0 for non-PLAIN kinds
  unsigned h = trap_hash(pc);
  for (unsigned n = 0; n < TRAP_IX_SIZE; n++, h = (h + 1) & (TRAP_IX_SIZE - 1))
    if (!tab[h]) { tab[h] = idx + 1; return; }
}
// Bring the index up to date with g_trap_n. Called by the builders, under gm_lock, once the
// entries are final. A SHRINK means a builder rolled back on an error path before publishing
// anything, so a full rebuild is both safe and rare.
static void trap_ix_sync(void) {
  if (g_trap_n < g_ix_n) {
    memset(g_ix_site, 0, sizeof g_ix_site);
    memset(g_ix_stub, 0, sizeof g_ix_stub);
    g_ix_n = 0;
  }
  for (int i = g_ix_n; i < g_trap_n; i++) {
    trap_ix_put(g_ix_site, g_trap[i].site, i);
    trap_ix_put(g_ix_stub, g_trap[i].stub, i);
  }
  g_ix_n = g_trap_n;
}

static struct x18_trap* trap_by(unsigned long pc, int by_stub) {
  const int* tab = by_stub ? g_ix_stub : g_ix_site;
  unsigned h = trap_hash(pc);
  for (unsigned n = 0; n < TRAP_IX_SIZE; n++, h = (h + 1) & (TRAP_IX_SIZE - 1)) {
    int v = tab[h];
    if (!v) return NULL;                                  // empty slot ends the probe
    struct x18_trap* t = &g_trap[v - 1];
    if ((by_stub ? t->stub : t->site) == pc) return t;
  }
  return NULL;
}

// `br x18` / `blr x18` would leave the stub without ever reaching its trailing brk, so a stub
// cannot service them; the handler performs the branch instead.
//
// This used to be a loose mask (`(w & 0xFE9FF000) == 0xD61F0000`) that also matched the
// pointer-authenticated variants, which the handler would then have executed as plain branches --
// silently stripping the auth. gmem_x18_classify() now reports those as UNSUP, and this asks
// vn_branch_reg_op() for the exact BR/BLR/RET encodings so the two agree.
static int insn_is_branch_reg(unsigned int w) { return vn_branch_reg_op(w) >= 0; }

int gmem_x18_trap_enter(unsigned long pc, unsigned long long* x, unsigned long* x18,
                        unsigned long long* saved,
                        unsigned long* next) {
  struct x18_trap* t = trap_by(pc, 0);
  if (!t) return 0;
  t->hits++;
  unsigned int w = t->insn;
  // BR/BLR/RET through x18. A stub cannot service these — the branch leaves the stub and its
  // trailing brk is never reached — but the handler can just do them: the destination IS the
  // value we are holding. Converting them matters because leaving them veneered would mix the
  // shared global with per-thread storage, and the two would disagree (an x18 read then returns
  // 0 and the guest dereferences it).
  if (insn_is_branch_reg(w)) {
    unsigned op = (w >> 21) & 3;                    // 0 = BR, 1 = BLR, 2 = RET
    if (op == 1) x[30] = t->site + 4;               // BLR links
    *next = *x18;
    return 1;
  }
  switch (t->kind) {
  case GMEM_X18_ADR: {
    unsigned long imm = (((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3);
    long simm = (long)(imm << 43) >> 43;
    (*x18) = (w & 0x80000000u) ? ((t->site & ~0xFFFUL) + ((unsigned long)simm << 12))
                                       : (t->site + (unsigned long)simm);
    *next = t->site + 4;
    return 1;
  }
  case GMEM_X18_LDRLIT: {
    long simm = ((long)((w >> 5) & 0x7FFFF) << 45) >> 45;
    unsigned long a = t->site + (unsigned long)(simm * 4);
    unsigned opc = w >> 30;
    if (opc == 0)      { unsigned int v; memcpy(&v, (void*)a, 4); (*x18) = v; }
    else if (opc == 1) { unsigned long v; memcpy(&v, (void*)a, 8); (*x18) = v; }
    else               { int v; memcpy(&v, (void*)a, 4); (*x18) = (unsigned long)(long)v; }
    *next = t->site + 4;
    return 1;
  }
  case GMEM_X18_TBZ: {
    long off = ((long)((w >> 5) & 0x3FFF) << 50) >> 50;
    unsigned bit = ((w >> 31) << 5) | ((w >> 19) & 31);
    int set = ((*x18) >> bit) & 1;
    int want = (w >> 24) & 1;                       // 0 = TBZ (branch if clear), 1 = TBNZ
    *next = (set == want) ? t->site + (unsigned long)(off * 4) : t->site + 4;
    return 1;
  }
  case GMEM_X18_CBZ: {
    long off = ((long)((w >> 5) & 0x7FFFF) << 45) >> 45;
    unsigned long v = (w & 0x80000000u) ? (*x18) : ((*x18) & 0xFFFFFFFFu);
    int want = (w >> 24) & 1;                       // 0 = CBZ, 1 = CBNZ
    *next = ((v != 0) == want) ? t->site + (unsigned long)(off * 4) : t->site + 4;
    return 1;
  }
  default:                                          // GMEM_X18_PLAIN: run the stub
    *saved = x[t->K];
    x[t->K] = (*x18);
    *next = t->stub;
    return 1;
  }
}

int gmem_x18_trap_leave(unsigned long pc, unsigned long long* x, unsigned long* x18,
                        unsigned long long* saved,
                        unsigned long* next) {
  struct x18_trap* t = trap_by(pc - 4, 1);          // pc is the brk AFTER the instruction
  if (!t) return 0;
  (*x18) = x[t->K];                         // harmless when the instruction only read it
  x[t->K] = *saved;
  *next = t->site + 4;
  return 1;
}

static void x18_trap_ranges(const struct x18_range* rs, int nr) {
  size_t sites = 0;
  for (int r = 0; r < nr; r++)
    for (size_t i = 0, nw = rs[r].n / 4; i < nw; i++) {
      unsigned f;
      if (!x18_is_code(rs[r].va + i * 4)) continue;      // constant data inside .text
      unsigned int w = xr_get(&rs[r], i);
      int k = gmem_x18_classify(w, &f);
      if (k == GMEM_X18_NONE) continue;
      // NOTE: register branches are fine here -- gmem_x18_trap_enter() performs BR/BLR/RET
      // itself (the destination IS the shadow value it is holding), so unlike a veneer or a
      // re-execution stub the trap path needs no scratch register at all. This used to refuse
      // them, which sent any image containing `br x18` back to the racy shared global.
      if (k == GMEM_X18_UNSUP) {
        if (g_x18_partial || g_code_n) {
          fprintf(stderr, "[gmem] x18: %#lx (%08x) cannot be trapped; SKIPPING THAT WORD and "
                          "trapping the rest of the image (%s)\n",
                  rs[r].va + i * 4, w, g_code_n ? "per-site fallback" : "RDX18_PARTIAL");
          continue;
        }
        fprintf(stderr, "[gmem] x18: %#lx (%08x) cannot be trapped either; leaving this image on "
                        "the old (racy) path\n", rs[r].va + i * 4, w);
        return;
      }
      sites++;
    }
  if (!sites) return;
  if (g_trap_n + (int)sites > X18_TRAP_MAX) {
    fprintf(stderr, "[gmem] x18: trap table full (%zu more sites)\n", sites);
    return;
  }

  unsigned long stubs = vn_reserve_stubs(sites * 2);  // two words per stub (see vn_reserve_stubs)
  if (!stubs) { fprintf(stderr, "[gmem] x18: no room for %zu trap stubs\n", sites); return; }

  unsigned long sp_ = stubs;
  int first = g_trap_n;
  for (int r = 0; r < nr; r++)
    for (size_t i = 0, nw = rs[r].n / 4; i < nw; i++) {
      unsigned f;
      if (!x18_is_code(rs[r].va + i * 4)) continue;      // constant data inside .text
      unsigned int w = xr_get(&rs[r], i);
      int k = gmem_x18_classify(w, &f);
      if (k == GMEM_X18_NONE) continue;
      if (k == GMEM_X18_UNSUP) continue;      // per-site fallback: counted as skipped above
      unsigned long site = rs[r].va + i * 4;
      unsigned K = vn_scratch(w, 32);
      if (!insn_is_branch_reg(w) && K > 31) {     // a register branch needs no scratch at all
        g_trap_n = first; fprintf(stderr, "[gmem] x18: no scratch for %08x\n", w); return; }
      struct x18_trap* t = &g_trap[g_trap_n++];
      t->site = site; t->insn = w; t->kind = (unsigned char)k; t->K = (unsigned char)K;
      t->fields = f; t->stub = 0;
      if (k == GMEM_X18_PLAIN && !insn_is_branch_reg(w)) {
        unsigned int w2 = w;
        for (int b = 0; b < 4; b++)
          if (f & (1u << b)) w2 = x18_set_field(w2, x18_pos[b], K);
        unsigned int stub[2] = { w2, BRK_IMM(GMEM_BRK_X18_LEAVE) };
        if (gmem_map_code((void*)sp_, stub, sizeof stub) != 0) {
          g_trap_n = first; fprintf(stderr, "[gmem] x18: cannot publish a trap stub\n"); return;
        }
        t->stub = sp_;
        sp_ += 8;
      }
      xr_put(&rs[r], i, BRK_IMM(GMEM_BRK_X18_ENTER));
    }
  g_vn_used += sites * 2 * 4;
  // The two capacities above fail in OPPOSITE ways and only one of them says so. Overflowing
  // X18_TRAP_MAX prints "trap table full" and drops the image back to the racy global -- loud.
  // Overflowing the INDEX prints nothing at all: trap_ix_put() silently discards, trap_by()
  // finds no empty slot to stop at, returns NULL, and the BRK aborts the process with no
  // diagnostic. So warn on approach, while there is still something to read.
  {
    static int warned;
    if (!warned && g_trap_n > (X18_TRAP_MAX * 3) / 4) {
      warned = 1;
      fprintf(stderr, "[gmem] x18: trap table %d/%d (%d%%) -- raise X18_TRAP_MAX **AND** "
                      "TRAP_IX_BITS together; TRAP_IX_BITS must stay >= 2x X18_TRAP_MAX or the "
                      "index overflows and the process aborts with no message\n",
              g_trap_n, X18_TRAP_MAX, g_trap_n * 100 / X18_TRAP_MAX);
    }
  }
  trap_ix_sync();                     // index the new entries -- MUST NOT be under g_verbose
  if (g_verbose)
    fprintf(stderr, "[gmem] x18: %zu site(s) put on the trap path across %d range(s)\n", sites, nr);
}

// Turn every veneered site into a trap site. Safe only while the guest is single-threaded, which
// is exactly when clone() calls it (the caller is inside the trap handler, so the one guest
// thread is stopped).
static int g_x18_mt;

void gmem_x18_multithread(void) {
  if (g_x18_mt) return;
  g_x18_mt = 1;

  // ---- THE DEFAULT: there is nothing to convert. ------------------------------------------
  // This conversion answered one question: "a veneer can only reach the single global
  // gmem_guest_x18, so what happens when a second guest thread exists?" A veneer now reaches
  // this thread's Darwin TSD slot instead (gmem_x18_tsd_init above), so the question is answered at
  // the veneer, and converting is pure loss: a veneer is `mrs tpidrro_el0` + `and` + a load,
  // a trap is a full Mach exception round-trip, and it was being applied to ~18670 sites in
  // libLLVM/libgallium — the hottest code the guest runs.
  //
  // It survived this long because it was also MASKING a real bug: vn_build had no case for
  // register branches, so `blr x18` got the plain shape and ran its epilogue on the callee's
  // leftovers. Converting replaced those broken veneers before they ever executed. That is
  // fixed at the source now (2c8d5cf): BLR gets a no-epilogue x16 veneer, and BR/RET send the
  // WHOLE IMAGE to the trap path from pass 0 of x18_neutralize_ranges, at rewrite time.
  //
  // This is a plain early return rather than a per-site filter, and that is deliberate. When it
  // was written, x18 still had two unreconciled homes (the TSD slot for veneers, gthread::x18
  // for the trap path), so converting a SUBSET of one image's sites would have left the halves
  // reading different memory — an earlier attempt at exactly that, converting only the
  // branch-register sites, is why this file grew the warning. 22129f1 has since given every
  // guest thread ONE home that both paths address, so mixing is no longer a correctness trap.
  // It is still not worth doing here: the whole point is that a veneer is strictly cheaper than
  // a trap, so the right subset to convert at clone() time is the empty one.
  //
  // MEASURED, the two binaries run interleaved so shared-machine load hit both sides equally
  // (844b29bd = converting, fb7984bb = not), osdemo under llvmpipe, 4 runs each:
  //   threaded (default LP_NUM_THREADS)   50.9/46.3/31.6/65.7s  ->  1.60/1.36/1.28/1.67s
  //   single-threaded (LP_NUM_THREADS=0)  1.51/1.52/1.53/1.51s  ->  1.50/1.52/1.52/1.52s
  //   tests/wl/x18thr.c                   36.0/30.6/39.1/39.4s  ->  0.062/0.065/0.066/0.064s
  // Output byte-identical (9bebbbab262da316) across all 16 osdemo runs, and x18thr reports
  // 0/800000 mismatches on both sides. The single-threaded row is the control: osdemo never
  // clones there, so neither binary reaches this function and the two must agree — they do.
  // Note what the threaded row was saying before: threading osdemo made it ~30x SLOWER than
  // not threading it, because every worker thread paid a Mach exception per x18 instruction.
  // No init call needed here: shim.c calls gmem_x18_tsd_init() before the guest runs, precisely
  // so that x18's home is chosen before any thread can bind its address. By the time a clone()
  // reaches this, the choice is long made.
  if (g_x18_tsd_off) return;

  // ---- from here down: RDX18_NOTSD=1 only -------------------------------------------------
  // Kept as the A/B PERFORMANCE reference — it is what the shim used to do to every image, so it
  // is how the cost of this conversion gets measured. It is NOT a correctness fallback, and must
  // not be described as one:
  //
  // RDX18_NOTSD IS RACY ACROSS THREADS AS OF 22129f1, and converting no longer fixes that. When
  // TSD storage is off, gmem_x18_slot() returns &gmem_guest_x18, so EVERY guest thread binds the
  // same word as its x18 home and the trap path is per-process rather than per-thread. Converting
  // used to make x18 per-thread because the trap path had its own gthread::x18 field; unifying the
  // two homes onto the veneers' slot removed the only thing that was making this mode correct.
  // MEASURED on /x18thr, 4 threads x 200k, and identical on plain 22129f1 (4a815a24) and on this
  // commit (dd20076d): RDX18_NOTSD=1 -> ~619k mismatches, FAIL. Before the unification the same
  // mode was 0 mismatches in 32.8s. So this path is now a benchmark knob, nothing more; if
  // gmem_x18_tsd_init() ever genuinely fails, this does NOT rescue the guest.
  //
  // RDX18_NOMT=1 skips the conversion, leaving the shared-global veneers in place — racy for the
  // same reason and by a wider margin. Under the TSD default the flag is moot: the early return
  // above already keeps the veneers, and those really are per-thread.
  if (getenv("RDX18_NOMT")) {
    fprintf(stderr, "[gmem] x18: RDX18_NOMT set — staying on the shared global (RACY)\n");
    return;
  }

  size_t nplain = 0, converted = 0, kept = 0;
  for (int i = 0; i < g_vnrec_n; i++) {
    unsigned f; int k = gmem_x18_classify(g_vnrec[i].insn, &f);
    if (k == GMEM_X18_PLAIN && !insn_is_branch_reg(g_vnrec[i].insn)) nplain++;
  }
  unsigned long stubs = nplain ? vn_reserve_stubs(nplain * 2) : 0;
  if (nplain && !stubs) {
    fprintf(stderr, "[gmem] x18: cannot reserve %zu trap stubs; x18 stays global and RACY across "
                    "threads\n", nplain);
    return;
  }
  unsigned long sp_ = stubs;

  for (int i = 0; i < g_vnrec_n; i++) {
    unsigned f;
    unsigned int w = g_vnrec[i].insn;
    int k = gmem_x18_classify(w, &f);
    if (k == GMEM_X18_NONE || k == GMEM_X18_UNSUP) { kept++; continue; }
    unsigned K = vn_scratch(w, 32);
    if (!insn_is_branch_reg(w) && K > 31) { kept++; continue; }
    if (g_trap_n >= X18_TRAP_MAX) { kept++; continue; }

    struct x18_trap* t = &g_trap[g_trap_n++];
    t->site = g_vnrec[i].site; t->insn = w; t->kind = (unsigned char)k;
    t->K = (unsigned char)K; t->fields = f; t->stub = 0;
    if (k == GMEM_X18_PLAIN && !insn_is_branch_reg(w)) {
      unsigned int w2 = w;
      for (int b = 0; b < 4; b++) if (f & (1u << b)) w2 = x18_set_field(w2, x18_pos[b], K);
      unsigned int stub[2] = { w2, BRK_IMM(GMEM_BRK_X18_LEAVE) };
      if (gmem_map_code((void*)sp_, stub, sizeof stub) != 0) { g_trap_n--; kept++; continue; }
      t->stub = sp_; sp_ += 8;
    }
    unsigned int brk = BRK_IMM(GMEM_BRK_X18_ENTER);
    if (gmem_map_code((void*)t->site, &brk, 4) != 0) { g_trap_n--; kept++; continue; }
    converted++;
  }
  g_vn_used += nplain * 2 * 4;
  if (kept)
    fprintf(stderr, "[gmem] x18: WARNING %zu site(s) could NOT be converted and still use the "
                    "shared global; they will disagree with the %zu converted ones\n",
            kept, converted);
  trap_ix_sync();                     // index the new entries -- MUST NOT be under g_verbose
  if (g_verbose || kept)
    fprintf(stderr, "[gmem] x18: %zu site(s) moved to the per-thread trap path for threading\n",
            converted);
}


// Convert every x18 site in these ranges to a trap. All or nothing, like the veneer path.

// ---- TLS read veneers ------------------------------------------------------------------------
// Replace `brk #(0x200+rt)` (which is what the svc/tpidr pass left where the guest wrote
// `mrs xrt, tpidr_el0`) with `b <veneer>`, and emit the 4-word veneer described above
// g_tls_tsd_off. Returns nothing: failure leaves the site on the trap path, which is exactly
// today's behaviour, so there is no all-or-nothing bail and no rollback to get wrong.
//
// SCOPE. This runs on the SAME ranges the x18 pass uses, i.e. SHF_EXECINSTR sections only. That
// is deliberate and it is strictly safer than the brk rewrite that precedes it: that one is
// whole-buffer by design, so a .rodata word that happens to encode `mrs x5, tpidr_el0` is already
// a brk today. Such a word does NOT get a branch planted over it here, because .rodata is not in
// these ranges. (A branch would be the worse corruption of the two: a brk in data is inert unless
// executed, whereas a `b` re-points nothing but still rewrites the constant either way. Keeping
// the aggressive rewrite out of data is the point.)
//
// rt == 18 is skipped: x18 is itself virtualized, so its value belongs in the shadow word, not in
// the hardware register — gmem_x18_classify already refuses this encoding for the same reason.
// rt == 31 is skipped: that is `mrs xzr, tpidr_el0`, a discard, and the veneer's `ldr` would read
// Rn=31 as SP rather than as zero.
//
// DECODED, not searched. This runs on EVERY WORD of every executable section of every image, twice
// (count, then emit), so the obvious `for (rt = 0..30) if (w == BRK_IMM(...))` is 31 comparisons
// per word and it is measurable: it cost `/bin/echo` on the Debian rootfs 15.4 -> 23.2 ms, i.e. it
// made short-lived processes 1.5x SLOWER while making long-lived ones 11x faster. Load-time work
// is paid by every process, including the ones with no time to earn it back.
static inline int tls_read_rt(unsigned int w) {
  if ((w & 0xFFE0001Fu) != 0xD4200000u) return -1;      // not a brk #imm16
  unsigned imm = (w >> 5) & 0xFFFFu;
  unsigned rt  = imm - BRK_TLS_READ;
  if (imm < BRK_TLS_READ || rt >= 31 || rt == 18) return -1;
  return (int)rt;
}

static void tls_veneer_ranges(const struct x18_range* rs, int nr) {
  if (!g_tls_tsd_off) return;                     // veneering off, or no TSD key: all traps

  size_t sites = 0;
  for (int r = 0; r < nr; r++)
    for (size_t i = 0, nw = rs[r].n / 4; i < nw; i++)
      if (tls_read_rt(xr_get(&rs[r], i)) >= 0) sites++;
  if (!sites) return;

  // 4 words per site, and the arena has to be within `b` range (+-128 MB) of EVERY range.
  //
  // THE ARENA POLICY IS THE SAME AS x18's, AND IT HAS TO BE. The first version of this used the
  // shared kernel-placed MAP_JIT arena unconditionally, and it silently did nothing where it
  // mattered most: glibc's ld.so reserves a DSO PROT_NONE and MAP_FIXEDs its text in, which sets
  // g_vn_hint, and the shared arena lands well over 128 MB away — so libc.so.6's 1485 sites
  // reported "no arena reachable" and stayed on the trap path while three small images were
  // veneered and the trap census did not move at all. Caught by the census, not by review.
  unsigned long arena;
  if (g_vn_hint) {
    unsigned long lo = rs[0].va, hi = rs[0].va + rs[0].n;
    for (int r = 1; r < nr; r++) {
      if (rs[r].va < lo) lo = rs[r].va;
      if (rs[r].va + rs[r].n > hi) hi = rs[r].va + rs[r].n;
    }
    arena = plain_arena_near(lo, hi - lo, sites * 16);
  } else {
    arena = vn_reserve_run(sites * 4);
  }
  int reachable = arena != 0;
  for (int r = 0; r < nr && reachable; r++)
    if (!b_in_range(rs[r].va, arena) ||
        !b_in_range(rs[r].va + rs[r].n, arena + sites * 16)) reachable = 0;
  if (!reachable) {
    if (g_verbose)
      fprintf(stderr, "[gmem] tls: no arena reachable from all %d range(s) (%zu sites); they stay "
                      "on the trap path\n", nr, sites);
    plain_finish();
    return;
  }

  unsigned long vn = arena;
  size_t done = 0, mix_ix = 0;
  for (int r = 0; r < nr; r++) {
    size_t nw = rs[r].n / 4;
    for (size_t i = 0; i < nw; i++) {
      int rt = tls_read_rt(xr_get(&rs[r], i));
      if (rt < 0) continue;
      unsigned long site = rs[r].va + i * 4;
      if (g_tls_mixn && (++mix_ix % (size_t)g_tls_mixn) == 0) continue;   // TEST MODE: trap it
      unsigned int v[4] = {
        E_MRS_TPIDRRO((unsigned)rt),
        E_AND_NOT7((unsigned)rt, (unsigned)rt),
        E_LDR_IMM((unsigned)rt, (unsigned)rt, g_tls_tsd_off),
        E_B((long)(site + 4) - (long)(vn + 12)),
      };
      if (!b_in_range(site, vn) || vn_write(vn, v, sizeof v) != 0) continue;  // stays trapped
      xr_put(&rs[r], i, E_B((long)vn - (long)site));
      vn += sizeof v;
      done++;
    }
  }
  if (!g_plain_base) g_vn_used += sites * 16;     // commit the shared run (plain arenas are ours)
  plain_finish();                                 // release it before the x18 pass places its own
  if (g_verbose || g_tls_mixn)
    fprintf(stderr, "[gmem] tls: %zu site(s) veneered, %zu left trapped, %zu KB of veneers\n",
            done, sites - done, sites * 16 / 1024);
}

static void x18_neutralize_ranges(const struct x18_range* rs, int nr,
                                  unsigned long arena_in, size_t arena_bytes) {
  gmem_x18_tsd_init();
  // BEFORE the x18 pass, on the same ranges. Order is not load-bearing (a TLS site is a `brk`,
  // which gmem_x18_classify returns NONE for, and the `b` this plants is equally NONE), but doing
  // it first means an x18 bail cannot take the TLS win down with it.
  tls_veneer_ranges(rs, nr);
  // ---- pass 0: can this image be veneered at all? -----------------------------------------
  // A veneer spills scratch registers through SP, and AArch64 faults on ANY SP-based access when
  // SP is not 16-byte aligned. Hand-written assembly genuinely runs that way -- pixman's aarch64
  // NEON prologue is `sub sp, sp, #0xe8` (232, misaligned by 8) followed by saves through x29 --
  // so such an image gets the trap path instead. Skipping it is NOT an option: pixman uses x18 as
  // an ordinary temporary in 563 places (and/orr/lsl/madd/mul), so unveneered it computes on a
  // register macOS is zeroing underneath it.
  //
  // MISALIGNED SP IS WHOLE-IMAGE, AND THAT IS A CORRECTNESS REQUIREMENT, NOT A POLICY. Do not
  // merge it with the branch case below. SP alignment is a property of the RUNNING image, not of
  // the one word that establishes it: once a function has left SP misaligned, a veneer ANYWHERE
  // in that image faults on its own spill. Per-site granularity cannot help, because the site
  // that faults is not the site that misaligned SP.
  //
  // `br`/`ret x18` is the opposite: it is a property of exactly ONE word, and the trap handler
  // services it borrowing NOTHING (gmem_x18_trap_enter performs the branch itself -- the
  // destination IS the shadow value it holds). So it is trapped PER SITE and the image's other
  // x18 sites keep the fast veneer path.
  //
  // Whole-image for the branch case was a POLICY, and it used to be a correctness requirement:
  // veneers kept x18 in this thread's TSD slot while the trap path kept its own copy in
  // gthread::x18, so mixing the two inside one image left the halves reading different storage.
  // Both paths now reach the same word (see the invariant note above gmem_x18_slot), which is what
  // RDX18_MIXN and tests/wl/x18mix.c exercise, and which is what makes this legal.
  //
  // WHAT IT IS WORTH, and why 7 instructions were the whole cost: libjavascriptcoregtk-4.1 holds
  // SEVEN `br x18` and libwebkit2gtk-4.1 TEN, and both are otherwise clean (unsup=0, misalignSP=0
  // by tools/x18check -paths). Whole-image put 8758 + 8908 = 17666 sites on the trap path -- most
  // of the trap table the handler linearly scans -- so every x18 site EXECUTION in JavaScriptCore's
  // compiler, GC and runtime cost a Mach exception plus that scan. Per-site trapping moves 17652
  // of those 17666 onto veneers and leaves 17 words trapped.
  size_t nbr = 0;                       // register branches through x18: trapped PER SITE
  unsigned long br_first = 0; unsigned int br_first_w = 0;
  for (int r = 0; r < nr; r++)
    for (size_t i = 0, nw = rs[r].n / 4; i < nw; i++) {
      if (!x18_is_code(rs[r].va + i * 4)) continue;      // constant data inside .text
      unsigned int w = xr_get(&rs[r], i);
      if (insn_misaligns_sp(w)) {
        if (g_verbose)
          fprintf(stderr, "[gmem] x18: %#lx (%08x) adjusts sp by a non-multiple of 16, so the "
                          "image can run with a misaligned sp, so this image's x18 sites go on "
                          "the trap path\n", rs[r].va + i * 4, w);
        x18_trap_ranges(rs, nr);
        return;
      }
      if (insn_unveneerable_x18_branch(w)) {
        if (!nbr) { br_first = rs[r].va + i * 4; br_first_w = w; }
        nbr++;
      }
    }
  int br_persite = 0;
  if (nbr) {
    // The two ways per-site trapping is refused. Both keep the WHOLE-image trap, never the racy
    // global: a register branch left in the register file is a branch through a word XNU zeroes.
    const char* why = 0;
    if (gmem_x18_brwhole())                       why = "RDX18_BRWHOLE=1, the A/B control";
    else if (g_trap_n + (int)nbr > X18_TRAP_MAX)  why = "the trap table has no room for them";
    if (why) {
      fprintf(stderr, "[gmem] x18: %zu register branch(es) through x18 (first %08x @ %#lx) stay "
                      "WHOLE-image trapped: %s\n", nbr, br_first_w, br_first, why);
      x18_trap_ranges(rs, nr);
      return;
    }
    br_persite = 1;
  }

  // EVERY WAY OUT OF PASSES 1 AND 2 THAT LEAVES THE IMAGE UNPATCHED IS A WAY OUT TO THE RACY
  // SHARED GLOBAL, and for an image we only reached because of a register branch that is STRICTLY
  // WORSE than the whole-image trap we would otherwise have taken: racy means raw hardware x18,
  // which XNU zeroes on every exception return to EL0, inside JavaScriptCore's compiler and GC.
  //
  // So per-site trapping is not allowed to be a one-way bet. Every such exit funnels through here
  // and takes the whole-image trap instead. Nothing is patched at any of these points (pass 2's
  // rollback restores the words and resets g_trap_n before jumping here), so x18_trap_ranges() is
  // walking the original instructions, exactly as it would have from pass 0.
  #define X18_BR_GIVE_UP() do {                                                                 \
      if (br_persite) {                                                                         \
        fprintf(stderr, "[gmem] x18: could not veneer the rest of this image, so the %zu "        \
                        "per-site register-branch trap(s) cannot stand alone; the WHOLE image "   \
                        "goes on the trap path instead of the racy global\n", nbr);              \
        x18_trap_ranges(rs, nr);                                                                \
      }                                                                                         \
      return;                                                                                   \
    } while (0)

  // ---- pass 1: is every site something we can handle? -------------------------------------
  size_t sites = 0, unsup_skipped = 0;
  for (int r = 0; r < nr; r++) {
    size_t nw = rs[r].n / 4;
    for (size_t i = 0; i < nw; i++) {
      unsigned f;
      if (!x18_is_code(rs[r].va + i * 4)) {              // constant data inside .text
        // THE ONE NUMBER THIS GATE HAS TO BE JUDGED ON. A word the code map calls data, that
        // nevertheless classifies as a REWRITEABLE x18 site, is either a constant that must not be
        // patched or a real instruction being abandoned to a register XNU zeroes -- and only the
        // second is a bug. Count them so the failure is visible in the log rather than inferred
        // from a corrupted hash three libraries later. Under `.eh_frame` alone libgmp reports 155
        // here; with the `.dynsym` STT_FUNC union it must report 0.
        if (g_verbose) {
          unsigned f2; int k2 = gmem_x18_classify(xr_get(&rs[r], i), &f2);
          if (k2 != GMEM_X18_NONE && k2 != GMEM_X18_UNSUP) g_code_skipped_sites++;
        }
        continue;
      }
      unsigned int w = xr_get(&rs[r], i);
      int k = gmem_x18_classify(w, &f);
      if (k == GMEM_X18_UNSUP) {
        // PER-SITE FALLBACK. One word we cannot classify costs ONE site, not the image. Safe only
        // because x18_is_code() has already removed the constant tables: without it this is the
        // recorded RDX18_PARTIAL incident, which planted `brk` over libcrypto's round constants.
        // Enabled when a code map exists; an image with no `.eh_frame` keeps whole-image bail.
        if (g_x18_partial || g_code_n) {
          unsup_skipped++;
          fprintf(stderr, "[gmem] x18: %#lx (%08x) cannot be veneered; SKIPPING THAT ONE SITE, the "
                          "rest of this image still goes on the per-thread veneer path (%s)\n",
                  rs[r].va + i * 4, w, g_code_n ? "per-site fallback" : "RDX18_PARTIAL");
          continue;
        }
        fprintf(stderr, "[gmem] x18: %#lx has an x18 operand in an encoding I cannot rewrite "
                        "(%08x); leaving this image on the old (racy) path\n",
                rs[r].va + i * 4, w);
        X18_BR_GIVE_UP();
      }
      if (k != GMEM_X18_NONE) sites++;
    }
  }
  if (unsup_skipped)
    fprintf(stderr, "[gmem] x18: %zu site(s) left on the racy global; %zu on the per-thread "
                    "veneer path\n", unsup_skipped, sites);
  if (!sites) X18_BR_GIVE_UP();

  // ---- pass 1.5: size the veneers, and find space they can all reach ----------------------
  unsigned int vbuf[32];
  size_t total = x18_measure_ranges(rs, nr);
  if (!total) X18_BR_GIVE_UP();                 // unrewritable: nothing patched, message already
  total /= 4;                                   // words, to match vn_build's return

  // A caller that reserved space adjacent to the image passes it here; that placement is what
  // makes `b` reachability structural rather than luck. Otherwise fall back to the shared arena,
  // which the kernel may well have placed more than 128 MB away.
  unsigned long arena = arena_in;
  if (arena) {
    if (arena_bytes < total * 4) {
      fprintf(stderr, "[gmem] x18: caller arena %zu B < %zu B needed; leaving this image on the "
                      "old (racy) path\n", arena_bytes, total * 4);
      X18_BR_GIVE_UP();
    }
  } else if (g_vn_hint) {
    unsigned long lo = rs[0].va, hi = rs[0].va + rs[0].n;
    for (int r = 1; r < nr; r++) {
      if (rs[r].va < lo) lo = rs[r].va;
      if (rs[r].va + rs[r].n > hi) hi = rs[r].va + rs[r].n;
    }
    arena = plain_arena_near(lo, hi - lo, total * 4);
    if (arena && g_verbose)
      fprintf(stderr, "[gmem] x18: plain veneer arena %#lx +%#zx next to %#lx +%#zx\n",
              arena, g_plain_len, lo, hi - lo);
  } else {
    arena = vn_reserve_run(total);
  }
  int reachable = arena != 0;
  for (int r = 0; r < nr && reachable; r++)
    if (!b_in_range(rs[r].va, arena) || !b_in_range(rs[r].va + rs[r].n, arena + total * 4))
      reachable = 0;
  // Whatever the arena came from, if `b` cannot reach it, try a kernel-walked hole beside the
  // range before giving up on veneers. This is the case the shared MAP_JIT arena loses: MAP_JIT
  // cannot take MAP_FIXED, so the kernel places it where it likes -- routinely past +/-128 MB
  // from the image that needs it -- and until now that image simply went racy. The hole search
  // does not care how the arena was originally obtained, only where the range is.
  if (!reachable) {
    unsigned long lo = rs[0].va, hi = rs[0].va + rs[0].n;
    for (int r = 1; r < nr; r++) {
      if (rs[r].va < lo) lo = rs[r].va;
      if (rs[r].va + rs[r].n > hi) hi = rs[r].va + rs[r].n;
    }
    size_t cap = (size_t)PAGE_UP(total * 4 < GPAGE ? GPAGE : total * 4);
    unsigned long a = arena_hole_near(lo, hi - lo, cap);
    if (a) {
      reachable = 1;
      for (int r = 0; r < nr && reachable; r++)
        if (!b_in_range(rs[r].va, a) || !b_in_range(rs[r].va + rs[r].n, a + total * 4))
          reachable = 0;
      if (reachable) {
        arena = a;
        g_plain_base = (unsigned char*)a; g_plain_len = cap; g_plain_rwx = 0;
        if (g_verbose)
          fprintf(stderr, "[gmem] x18: veneer arena %#lx +%#zx from a kernel-walked hole beside "
                          "%#lx +%#zx\n", a, cap, lo, hi - lo);
      } else {
        mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)a, cap);
      }
    }
  }
  if (!reachable) {
    // TRAP the range rather than abandon it. Trap stubs are entered by writing the PC in the
    // exception handler, never by a `b` planted in the image, so they need no reachability at all
    // -- which is precisely the thing that failed here. And every site is known trappable: pass 1
    // above already rejected the encodings that cannot be re-executed out of line, so reaching
    // this point means the only obstacle was where the veneers would live.
    //
    // The old behaviour was to leave the range on gmem_guest_x18, ONE process-global word shared
    // by every guest thread. That is not a degraded mode, it is a broken one: a single raw write
    // to x18 desynchronises every later shadow read, and in a browser run five such ranges (39
    // sites) were enough to fault WebKit on `ldr q28, [x18, x21]` with x18 reading 0. Trapping is
    // slower than a veneer but it is CORRECT, and 39 sites is not a hot path.
    {
      unsigned long dlo = rs[0].va, dhi = rs[0].va + rs[0].n;
      for (int r = 1; r < nr; r++) {
        if (rs[r].va < dlo) dlo = rs[r].va;
        if (rs[r].va + rs[r].n > dhi) dhi = rs[r].va + rs[r].n;
      }
      fprintf(stderr, "[gmem] x18: NO ARENA for range %#lx +%#lx (span %lu KB, %zu sites, %zu KB "
                      "of veneers) — no free hole within +/-%lu MB\n",
              dlo, dhi - dlo, (dhi - dlo) / 1024, sites, total * 4 / 1024,
              ARENA_WINDOW >> 20);
    }
    fprintf(stderr, "[gmem] x18: no veneer arena reachable from all %d range(s) (%zu sites, "
                    "%zu KB); TRAPPING the range instead — correct, and no `b` reachability "
                    "needed\n",
            nr, sites, total * 4 / 1024);
    plain_finish();
    x18_trap_ranges(rs, nr);                    // handles the per-site branches too
    return;
  }

  // ---- pass 2: emit the veneers and plant the branches ------------------------------------
  // Every patched word is remembered so that ANY later failure restores the image exactly.
  // (Veneers already written to JIT memory are then unreachable, and harmless.)
  struct undo { int r; size_t i; unsigned int w; };
  struct undo* undo = malloc(sites * sizeof *undo);
  if (!undo) { fprintf(stderr, "[gmem] x18: out of memory for %zu undo entries\n", sites);
               plain_finish(); X18_BR_GIVE_UP(); }

  // TEST-ONLY per-site mixing (RDX18_MIXN). Reserve stubs for the subset that will be trapped,
  // up front; if the arena cannot be had, mixing silently turns off and the image is veneered as
  // usual, which is the safe direction.
  size_t mix_ix = 0, mix_done = 0, mix_max = 0, nv_max = 0;
  unsigned long mix_stub = 0, nv_stub = 0;
  int mix_first_trap = g_trap_n, vn_committed = 0;
  if (g_x18_mixn) {
    // vn_reserve_run() keeps handing out the SAME address until the previous run is committed
    // into g_vn_used, so commit the veneer run first or the stubs land on top of the veneers.
    if (!arena_in && !g_plain_base) { g_vn_used += total * 4; vn_committed = 1; }
    mix_max = sites / (size_t)g_x18_mixn + 1;
    if (g_trap_n + (int)mix_max > X18_TRAP_MAX) mix_max = 0;
    mix_stub = mix_max ? vn_reserve_stubs(mix_max * 2) : 0;
    if (!mix_stub) {
      fprintf(stderr, "[gmem] x18: RDX18_MIXN: no room for %zu trap stubs; mixing OFF\n", mix_max);
      mix_max = 0;
    } else {
      g_vn_used += mix_max * 2 * 4;
      fprintf(stderr, "[gmem] x18: RDX18_MIXN=%d — every %dth of %zu site(s) goes on the TRAP "
                      "path, the rest are veneered (TEST MODE)\n",
              g_x18_mixn, g_x18_mixn, sites);
    }

  }

  // vn_reserve_run() hands out the same address until the previous run is committed,
  // so commit the veneer run before taking stub space or the stubs land on the veneers.
  if ((g_x18_partial || g_code_n) && !arena_in && !g_plain_base && !vn_committed) {
    g_vn_used += total * 4; vn_committed = 1;
  }
  // A site the veneer emitter REFUSES also needs a trap stub, and those exist regardless of
  // RDX18_MIXN. Without a pool of its own the stub write lands at address 0, gmem_map_code
  // fails, and the site falls through to the veneer that already refused it -- rolling the
  // whole image back to the racy global, the very outcome this is meant to avoid.
  if (g_x18_partial || g_code_n) {
    nv_max = 64;                                 // refusals are rare: 3 in the whole of Blender 5
    if (g_trap_n + (int)nv_max > X18_TRAP_MAX) nv_max = 0;
    nv_stub = nv_max ? vn_reserve_stubs(nv_max * 2) : 0;
    if (nv_stub) g_vn_used += nv_max * 2 * 4; else nv_max = 0;
  }

  unsigned long vn = arena;
  size_t done = 0, br_done = 0, nv_done = 0;
  for (int r = 0; r < nr; r++) {
    size_t nw = rs[r].n / 4;
    for (size_t i = 0; i < nw; i++) {
      unsigned f;
      if (!x18_is_code(rs[r].va + i * 4)) continue;      // constant data inside .text
      unsigned int w = xr_get(&rs[r], i);
      int k = gmem_x18_classify(w, &f);
      if (k == GMEM_X18_NONE) continue;
      if (k == GMEM_X18_UNSUP) continue;      // per-site fallback: measured as skipped too

      unsigned long site = rs[r].va + i * 4;

      // ---- this site goes on the trap path instead of getting a veneer --------------------
      // Two reasons, one mechanism. (1) THE REAL ONE: `br`/`ret x18`, which no veneer can service
      // because a jump-table dispatch guarantees no register dead -- measured across all 7 sites
      // in libjavascriptcoregtk, where x17 is live at 5 (the branch TARGETS load through it) and
      // x16 at 1. This is the cheapest case the trap path has: insn_is_branch_reg(w) is true, so
      // needs_stub is false and the site costs one `brk` plus one g_trap[] entry, no stub bytes.
      // (2) TEST-ONLY: RDX18_MIXN's every-nth site, which exists to prove the veneer's x18 home
      // and the handler's x18 home are the same word.
      int br_site  = br_persite && insn_unveneerable_x18_branch(w) && g_trap_n < X18_TRAP_MAX;
      int mix_site = !br_site && mix_max && mix_done < mix_max &&
                     (++mix_ix % (size_t)g_x18_mixn) == 0;
      // Try the veneer BEFORE deciding, so a site the emitter refuses takes the trap path rather
      // than rolling the whole image back to the racy global. Mixing both paths inside one image
      // is exactly what RDX18_MIXN exists to test, and tests/wl/x18mix.c reports 0 mismatches only
      // if the two paths really are the same word.
      int words = vn_build(vbuf, (int)(sizeof vbuf / 4), vn, site, w, k, f);
      int no_veneer = !words && (g_x18_partial || g_code_n) && g_trap_n < X18_TRAP_MAX && nv_done < nv_max;
      if (br_site || mix_site || no_veneer) {
        unsigned K = vn_scratch(w, 32);
        int needs_stub = (k == GMEM_X18_PLAIN && !insn_is_branch_reg(w));
        if (!needs_stub || K <= 31) {
          struct x18_trap* t = &g_trap[g_trap_n];
          t->site = site; t->insn = w; t->kind = (unsigned char)k;
          t->K = (unsigned char)K; t->fields = f; t->stub = 0;
          int ok = 1;
          if (needs_stub) {
            unsigned int w2 = w;
            for (int b = 0; b < 4; b++)
              if (f & (1u << b)) w2 = x18_set_field(w2, x18_pos[b], K);
            unsigned int stub[2] = { w2, BRK_IMM(GMEM_BRK_X18_LEAVE) };
            unsigned long sb = no_veneer ? nv_stub + nv_done * 8 : mix_stub + mix_done * 8;
            if (gmem_map_code((void*)sb, stub, sizeof stub) != 0) ok = 0;
            else t->stub = sb;
          }
          if (ok) {
            g_trap_n++;
            // Counted apart from mix_done on purpose: mix_done is BOTH the RDX18_MIXN budget and
            // the stub-array index, and a register branch needs no stub. Sharing the counter would
            // silently spend one test-mode stub slot per branch site.
            if (br_site) br_done++; else if (!no_veneer) mix_done++;
            if (no_veneer) nv_done++;
            undo[done].r = r; undo[done].i = i; undo[done].w = w; done++;
            xr_put(&rs[r], i, BRK_IMM(GMEM_BRK_X18_ENTER));
            continue;                                // no veneer for this one
          }
        }
        // could not trap it: fall through and veneer it, which is always safe
      }

      if (!words || !b_in_range(site, vn) ||
          vn_write(vn, vbuf, (size_t)words * 4) != 0) {
        fprintf(stderr, "[gmem] x18: veneer for %08x at %#lx failed; rolling back %zu patched "
                        "site(s), this image keeps the old (racy) behaviour\n", w, site, done);
        while (done--) xr_put(&rs[undo[done].r], undo[done].i, undo[done].w);
        g_trap_n = mix_first_trap;      // the mixed/per-site trap entries point at restored words
        free(undo);
        plain_finish();
        X18_BR_GIVE_UP();
      }
      undo[done].r = r; undo[done].i = i; undo[done].w = w; done++;
      xr_put(&rs[r], i, E_B((long)vn - (long)site));
      vn_record(vn, vn + (unsigned long)words * 4, site, w);
      vn += (unsigned long)words * 4;
    }
  }
  if (!vn_committed && !arena_in && !g_plain_base) g_vn_used += total * 4;  // commit shared run
  plain_finish();
  free(undo);
  // trap_ix_sync() MUST NOT be under g_verbose: an unindexed g_trap[] entry makes trap_by()
  // return NULL and the BRK abort the process with no diagnostic at all.
  if (mix_done || br_done) trap_ix_sync();
  if (nv_done)
    fprintf(stderr, "[gmem] x18: %zu site(s) the veneer emitter refused were TRAPPED per "
                    "site; the rest of this image is veneered (was: whole image racy)\n",
            nv_done);
  if (br_done)
    fprintf(stderr, "[gmem] x18: %zu register branch(es) trapped PER SITE, %zu site(s) veneered "
                    "in the same image (was: all %zu trapped)\n",
            br_done, done - mix_done - br_done, done);
  if (mix_done)
    fprintf(stderr, "[gmem] x18: RDX18_MIXN: %zu site(s) trapped, %zu veneered, IN THE SAME "
                    "IMAGE\n", mix_done, done - mix_done);
  if (g_verbose)
    fprintf(stderr, "[gmem] x18: %zu site(s) veneered across %d range(s), %zu KB of veneers\n",
            done - mix_done - br_done, nr, total * 4 / 1024);
  #undef X18_BR_GIVE_UP
}

// Single-range convenience wrapper (the elfload per-section path and the tests).
static void x18_neutralize(unsigned char* buf, size_t n, unsigned long va) {
  struct x18_range r = { buf, n, va };
  x18_neutralize_ranges(&r, 1, 0, 0);
}

static inline unsigned int rewrite_one(unsigned int w) {
  if (w == INSN_SVC0)                              return BRK_IMM(0);
  if ((w & INSN_MASK_RT) == INSN_MSR_TPIDR)        return BRK_IMM(BRK_TLS_WRITE + (w & 0x1F));
  if ((w & INSN_MASK_RT) == INSN_MRS_TPIDR)        return BRK_IMM(BRK_TLS_READ  + (w & 0x1F));
  if ((w & INSN_MASK_RT) == INSN_MRS_CTR)          return BRK_IMM(BRK_CTR_READ  + (w & 0x1F));
  if ((w & INSN_MASK_RT) == INSN_MRS_DCZID)        return BRK_IMM(BRK_MRS_DCZID + (w & 0x1F));
  if ((w & INSN_MASK_RT) == INSN_DC_ZVA)           return BRK_IMM(BRK_DC_ZVA    + (w & 0x1F));
  return w;
}

void gmem_rewrite_exec_insns(unsigned char* buf, size_t len) {
  gmem_rewrite_exec_insns_va(buf, len, 0);                  // va == 0: no x18 pass (see below)
}

void gmem_rewrite_exec_insns_va(unsigned char* buf, size_t len, unsigned long va) {
  if (!buf || len < 4) return;
  size_t n = len & ~(size_t)3;
  if ((((uintptr_t)buf) & 3) == 0) {                        // aligned: plain word array
    unsigned int* w = (unsigned int*)buf;
    size_t nw = n / 4;
    for (size_t i = 0; i < nw; i++) {
      unsigned int r = rewrite_one(w[i]);
      if (r != w[i]) w[i] = r;
    }
  } else {                                                  // unaligned staging buffer
    for (size_t o = 0; o < n; o += 4) {
      unsigned int w, r;
      memcpy(&w, buf + o, 4);
      r = rewrite_one(w);
      if (r != w) memcpy(buf + o, &r, 4);
    }
  }
  if (va) x18_neutralize(buf, n, va);
}


struct gm_sect { size_t bo, len; };
static int mapped_code_sections(int fd, long off, size_t rlen, struct gm_sect* out, int max);
#define GM_MAX_CODE_SECTS  64

// --- code the guest makes executable at RUNTIME ----------------------------------------------
// Instruction rewriting normally happens when an IMAGE IS LOADED. Anything the guest makes
// executable later — a JIT, a trampoline, a MAP_FIXED of its own text — never passed through it,
// so it kept raw `svc` and `mrs tpidr_el0`. That is not theoretical: WebKit dies on
//
//     ldr x9, [x9, x0]   with x9 == 0x2009      (macOS's raw TPIDR_EL0)
//
// having executed an `mrs x9, tpidr_el0` we never rewrote. It slipped through because this binary
// carries com.apple.security.cs.allow-unsigned-executable-memory, so a plain mmap/mprotect with
// PROT_EXEC simply SUCCEEDS — no MAP_JIT, no staging, no rewrite.
//
// So rewrite at the moment memory becomes executable — and that MUST include the x18 pass.
//
// It originally did not, on the reasoning that a veneer replaces an instruction with a branch to
// an out-of-line stub, and a JIT that later patched, moved or freed its own code would be left
// branching into a stub that no longer described it. That caution was misplaced here and the
// omission was itself a bug: LLVM's AArch64 backend reserves x18 only for Darwin and Windows
// triples, so for aarch64-linux it allocates x18 as an ordinary scratch register — and macOS
// clobbers it behind the guest's back. llvmpipe's generated shaders duly faulted on it:
//
//     [FAULT] bad access fault_addr=0x10 insn=3cab6a40   str q0, [x18, x11]
//             in (guest JIT: anonymous executable) + 0xbc4
//
// Note it is a STORE (bits[23:22]=0b10; a load would be 0b11). That matters: the JIT is SPILLING
// vector registers through x18, so a clobbered x18 means wild 16-byte WRITES to low addresses.
// It only presents as a clean fault because 0x10 happens to be unmapped — with a plausible x18
// it would be silent memory corruption. fault_addr 0x10 = x18(0) + x11(0x10) also confirms x18
// reads as 0. And this is on the DEFAULT path, not an LP_NUM_THREADS=0 side path, so the x18
// pass on runtime-exec pages is mandatory for llvmpipe to render at all.
//
// The stale-stub worry does not apply to what actually publishes code here: Mesa uses MCJIT,
// whose SectionMemoryManager is publish-once — mmap anon RW, write the section, flip to R+X, and
// never write that page again.
// Returns 1 if the region ends up R+X (whether or not anything needed rewriting), 0 if it does
// not. The W^X exec-flip path below needs that distinction: it may only record a page as
// executable in its bitmap if the page really is executable, or the next write fault decides the
// page is "already writable" and declines a fault that was in fact ours to serve.
// ---------------------------------------------------------------------------------------------
// SYSCALL FAST PATH FOR LEAF WRAPPERS — why fastcall patched 21 sites and needed to patch 288
//
// fastcall.c rewrites `brk #0` (which is what the guest's `svc #0` became) into `bl <thunk>`, so
// the syscall runs in-thread instead of costing a cross-thread Mach exception. MEASURED: 9443 ns
// per syscall on the trap path against 93 ns for the same call natively — a 101x tax.
//
// But `bl` clobbers x30, so fastcall_patch_ex() may only fire where x30 is provably dead. In
// libc.so.6 that is NEVER: a glibc syscall wrapper is a leaf — `mov x8,#nr; svc #0; ret` — and
// x30 holds the live return address across the svc. MEASURED on this rootfs:
//
//     [fastcall] glibc DSO 0x1166e0000 +0x1c0000: patched=0 skipped(x30-live)=288
//
// Zero of 288. So the `bl` form can never reach the library where essentially every syscall a
// real program makes actually lives.
//
// A per-site stub reached by `b` fixes it, because `b` writes no link register at all. The stub
// saves the two registers it needs (x30 for the guest, x16 to hold the thunk address), calls the
// existing thunk, restores them, and branches back to the instruction after the site:
//
//     stub:  sub  sp, sp, #16
//            stp  x30, x16, [sp]
//            ldr  x16, <literal>
//            blr  x16                 ; the thunk saves/restores everything else
//            ldp  x30, x16, [sp]
//            add  sp, sp, #16
//            b    site+4
//            .quad thunk
//
// `blr` rather than `bl` so the thunk does not have to be within the stub's +-128 MB. The stub
// itself does, and so does the site — plain_arena_near() is the same allocator the x18 veneers
// use for exactly that reason.
//
// The 16 bytes of guest stack are safe at a syscall boundary specifically: on real Linux the
// kernel writes a signal frame below SP on any signal delivery, so no correct program keeps live
// data there across an svc. (AArch64 has no red zone.)
#define FC_STUB_BYTES 48
static int fc_stub_patch(unsigned char* stg, size_t rlen, unsigned long addr,
                         const struct gm_sect* sect, int nsect,
                         unsigned long thunk, int* out_skipped) {
  if (out_skipped) *out_skipped = 0;
  if (!stg || !thunk) return 0;

  struct gm_sect whole = { 0, rlen };
  const struct gm_sect* sc = (nsect > 0) ? sect : &whole;
  int nsc = (nsect > 0) ? nsect : 1;

  long nsite = 0;
  for (int i = 0; i < nsc; i++) {
    const unsigned int* c = (const unsigned int*)(stg + sc[i].bo);
    for (size_t j = 0; j + 4 <= sc[i].len; j += 4)
      if (c[j / 4] == BRK_IMM(0)) nsite++;
  }
  if (!nsite) return 0;

  unsigned long arena = plain_arena_near(addr, rlen, (size_t)nsite * FC_STUB_BYTES);
  if (!arena) {
    if (g_verbose)
      fprintf(stderr, "[fastcall] no arena within b range of %#lx for %ld stub(s)\n", addr, nsite);
    if (out_skipped) *out_skipped = (int)nsite;
    return 0;
  }

  int done = 0, skipped = 0;
  unsigned long next = arena;
  for (int i = 0; i < nsc; i++) {
    unsigned int* c = (unsigned int*)(stg + sc[i].bo);
    for (size_t j = 0; j + 4 <= sc[i].len; j += 4) {
      if (c[j / 4] != BRK_IMM(0)) continue;
      // One shared rule for both patchers — see fastcall_denied_site().
      if (fastcall_denied_site(c, (long)(sc[i].len / 4), (long)(j / 4))) { skipped++; continue; }
      unsigned long site = addr + sc[i].bo + j;
      unsigned long sb   = next;
      long d_to_stub = (long)sb - (long)site;
      long d_back    = (long)(site + 4) - (long)(sb + 24);
      if ((d_to_stub & 3) || d_to_stub < -(1L << 27) || d_to_stub >= (1L << 27) ||
          (d_back & 3)    || d_back    < -(1L << 27) || d_back    >= (1L << 27)) {
        if (g_verbose && !skipped)
          fprintf(stderr, "[fastcall] RANGE: site=%#lx stub=%#lx arena=%#lx d=%ld\n",
                  site, sb, arena, d_to_stub);
        skipped++; continue;                       // out of b range: leave it on the trap path
      }
      unsigned int w[FC_STUB_BYTES / 4];
      w[0]  = 0xD10043FFu;                         // sub  sp, sp, #16
      w[1]  = 0xA90043FEu;                         // stp  x30, x16, [sp]
      w[2]  = 0x58000000u | (6u << 5) | 16u;       // ldr  x16, <literal at +32>
      w[3]  = 0xD63F0200u;                         // blr  x16
      w[4]  = 0xA94043FEu;                         // ldp  x30, x16, [sp]
      w[5]  = 0x910043FFu;                         // add  sp, sp, #16
      w[6]  = 0x14000000u | ((unsigned int)(d_back >> 2) & 0x03FFFFFFu);   // b site+4
      w[7]  = 0xD503201Fu;                         // nop (keeps the literal 8-byte aligned)
      memcpy(&w[8], &thunk, 8);                    // .quad thunk
      w[10] = 0xD503201Fu;
      w[11] = 0xD503201Fu;
      // vn_write, not gmem_map_code: plain_arena_near() hands back a PLAIN (non-MAP_JIT) arena,
      // where gmem_map_code's JIT write-protect toggle fails outright. vn_write picks the right
      // one, and plain_finish() below flips the arena executable and invalidates the icache.
      if (vn_write(sb, w, sizeof w) != 0) {
        if (g_verbose && !skipped)
          fprintf(stderr, "[fastcall] stub write failed at %#lx (arena %#lx)\n", sb, arena);
        skipped++; continue;
      }
      c[j / 4] = 0x14000000u | ((unsigned int)(d_to_stub >> 2) & 0x03FFFFFFu);  // b stub
      next += FC_STUB_BYTES;
      done++;
    }
  }
  plain_finish();                                // make the stub arena executable + flush icache
  if (out_skipped) *out_skipped = skipped;
  return done;
}

static int rewrite_runtime_exec(void* addr, size_t len, int fd, long off) {
  if (!addr || !len) return 0;
  // RDNOJITREWRITE=1 disables rewriting of ANONYMOUS regions (guest-generated code) while leaving
  // file-backed ones alone. It exists to answer one question definitively: is rewriting the JIT's
  // own output what breaks llvmpipe? Without a switch, "my new code is the bug" stays a theory.
  {
    static int off_anon = -1;
    if (off_anon < 0) off_anon = getenv("RDNOJITREWRITE") != 0;
    if (off_anon && fd < 0) {
      if (g_verbose)
        fprintf(stderr, "[gmem] runtime-exec: SKIPPED %p +%#zx (RDNOJITREWRITE)\n", addr, len);
      if (mprotect(addr, len, PROT_READ | PROT_EXEC) != 0) {
        fprintf(stderr, "[gmem] runtime-exec: mprotect R+X failed: %s\n", strerror(errno));
        return 0;
      }
      sys_icache_invalidate(addr, len);
      return 1;
    }
  }
  // The region must be writable to be patched. It is plain (non-JIT) memory here, so a normal
  // mprotect gets us there and back.
  if (mprotect(addr, len, PROT_READ | PROT_WRITE) != 0) {
    fprintf(stderr, "[gmem] runtime-exec: cannot make %p +%#zx writable to rewrite it: %s\n",
            addr, len, strerror(errno));
    return 0;
  }
  // Scope to real code when we can. A 54 MB library text segment is mostly NOT instructions, and
  // rewriting its co-located .rodata plants brk over constants — which showed up as 67 syscall
  // traps carrying a POINTER in x8 instead of a syscall number. Anonymous regions (fd < 0) are
  // the guest's own generated code and have no section table, so there the whole region is all
  // we can do.
  struct gm_sect sect[GM_MAX_CODE_SECTS];
  int nsect = (fd >= 0) ? mapped_code_sections(fd, off, len, sect, GM_MAX_CODE_SECTS) : -1;
  if (nsect > 0) {
    // File-backed: scope to real code, and keep the historical no-x18 behaviour here — these are
    // ordinary DSO text segments that the load-time path already handles when it can.
    for (int i = 0; i < nsect; i++)
      gmem_rewrite_exec_insns((unsigned char*)addr + sect[i].bo, sect[i].len);
    // AND THE FAST PATH. Everything above turned `svc #0` into `brk #0`, which costs a
    // cross-thread Mach exception round trip -- MEASURED at 9443 ns/call against 93 ns native,
    // a 101x tax on every syscall. fastcall turns the same site into `bl <thunk>`, an in-thread
    // call. It was only ever applied in elfload.c, which sees the main binary and the
    // interpreter; libc.so.6 is mapped HERE, by the guest's own ld.so, and libc is where
    // essentially every syscall a real program makes lives. So this is where it has to happen.
    { unsigned long th = fastcall_thunk_once();
      if (th) { int pat = 0, sk = 0;
        for (int i = 0; i < nsect; i++)
          pat += fastcall_patch_ex((unsigned char*)addr + sect[i].bo, sect[i].len, th, &sk);
        if (g_verbose && (pat || sk))
          fprintf(stderr, "[fastcall] runtime-exec %p: patched=%d skipped(x30-live)=%d\n",
                  addr, pat, sk);
      } }
  } else {
    // Anonymous: this IS guest-generated code, and it needs the x18 pass as much as any image.
    // Passing the region's real address enables it (va == 0 is what disables it).
    // The veneers go in a plain arena mmap'd right NEXT TO this region, because `b` has to
    // reach them: the shared arena is MAP_JIT, MAP_JIT cannot be combined with MAP_FIXED, so
    // the kernel places it where it likes — routinely outside b's +/-128MB. Without this hint
    // the arena is unreachable, x18_neutralize_ranges bails, and every JIT x18 site is left
    // RAW. That is exactly how the guest ends up storing through an XNU-zeroed x18.
    // A plain (non-MAP_JIT) arena is legitimate here: the binary carries
    // com.apple.security.cs.allow-unsigned-executable-memory.
    // RDX18_NOPLAIN=1 restores the old shared-arena behaviour, for A/B only.
    static int plain = -1;
    if (plain < 0) plain = getenv("RDX18_NOPLAIN") == 0;
    if (plain) g_vn_hint = (unsigned long)(uintptr_t)addr;
    gmem_rewrite_exec_insns_va((unsigned char*)addr, len, (unsigned long)(uintptr_t)addr);
    g_vn_hint = 0;
    { unsigned long th = fastcall_thunk_once();
      if (th) { int sk = 0;
        int pat = fastcall_patch_ex(addr, len, th, &sk);
        if (g_verbose && (pat || sk))
          fprintf(stderr, "[fastcall] runtime-exec(anon) %p: patched=%d skipped(x30-live)=%d\n",
                  addr, pat, sk);
      } }
  }
  if (mprotect(addr, len, PROT_READ | PROT_EXEC) != 0) {
    fprintf(stderr, "[gmem] runtime-exec: cannot re-protect %p +%#zx as R+X: %s\n",
            addr, len, strerror(errno));
    return 0;
  }
  sys_icache_invalidate(addr, len);
  if (g_verbose)
    fprintf(stderr, "[gmem] runtime-exec: rewrote %p +%#zx (guest made it executable)\n",
            addr, len);
  return 1;
}

// --- loaded-image map (diagnostics) ---------------------------------------------------------
// Recorded for every file-backed executable mapping, so a fault can name the library and the
// offset inside it. macOS's F_GETPATH turns the mapping's fd straight into a path.
#define GM_IMG_MAX 1024
struct gm_image { unsigned long base, len; char path[256]; };
static struct gm_image g_img[GM_IMG_MAX];
static int             g_img_n;

static void img_record(unsigned long base, unsigned long len, int fd) {
  if (g_img_n >= GM_IMG_MAX) return;
  struct gm_image* im = &g_img[g_img_n];
  im->base = base; im->len = len; im->path[0] = 0;
  char buf[1024];
  if (fd < 0) { snprintf(im->path, sizeof im->path, "(guest JIT: anonymous executable)"); }
  else if (fcntl(fd, F_GETPATH, buf) == 0) {
    const char* slash = strrchr(buf, '/');
    snprintf(im->path, sizeof im->path, "%s", slash ? slash + 1 : buf);
  }
  g_img_n++;
}

const char* gmem_image_for(unsigned long pc, unsigned long* base) {
  for (int i = 0; i < g_img_n; i++)
    if (pc >= g_img[i].base && pc < g_img[i].base + g_img[i].len) {
      if (base) *base = g_img[i].base;
      return g_img[i].path[0] ? g_img[i].path : "(unnamed mapping)";
    }
  return NULL;
}

// --- x18 on the guest-mmap path ----------------------------------------------------------------
// elfload.c veneers the main binary and the interpreter, because it has their section tables and
// so knows exactly which bytes are code. Everything ELSE the dynamic linker pulls in at runtime --
// libGL, libX11, Mesa's software rasterizer (6001 x18 sites), libLLVM (13735) -- arrives here
// instead, as an ordinary guest mmap, and used to be rewritten with va == 0, i.e. with NO x18 pass
// at all. Those images were therefore left exposed to macOS zeroing x18 behind the guest's back
// (see KNOWN_BUGS.md), which is survivable for something like libX11 that barely touches x18 and
// immediately fatal for a software rasterizer that spends every cycle in it.
//
// The mapping carries the fd it came from, so the section table is recoverable: pread the ELF and
// section headers and rewrite ONLY SHF_EXECINSTR bytes. That scoping is not cosmetic -- an R-X
// segment also holds .rodata, and an x18 rewrite is aggressive enough (every instruction naming
// x18, not a handful of exact encodings) that a constant which happens to look like one would
// otherwise be replaced by a branch.
typedef struct { uint32_t e_ident4; uint8_t cls, data, ver, osabi, pad[8];
                 uint16_t e_type, e_machine; uint32_t e_version;
                 uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags;
                 uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
               } gm_ehdr;
typedef struct { uint32_t sh_name, sh_type; uint64_t sh_flags, sh_addr, sh_offset, sh_size;
                 uint32_t sh_link, sh_info; uint64_t sh_addralign, sh_entsize; } gm_shdr;

typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr,
                 p_filesz, p_memsz, p_align; } gm_phdr;

typedef struct { uint32_t st_name; uint8_t st_info, st_other; uint16_t st_shndx;
                 uint64_t st_value, st_size; } gm_sym;

#define GM_SHF_ALLOC      0x2
#define GM_SHF_EXECINSTR  0x4
#define GM_SHT_SYMTAB       2
#define GM_SHT_NOBITS       8
#define GM_SHT_DYNSYM      11
#define GM_STT_FUNC         2
#define GM_PT_LOAD          1
#define GM_PT_GNU_EH_FRAME  0x6474e550

// Build the CODE map for this image from `.eh_frame` (see the long note above x18_is_code).
//
// PT_GNU_EH_FRAME points at `.eh_frame_hdr`, whose binary-search table names every FDE; an FDE's
// (pc_begin, pc_range) is the exact extent of one real function. A constant table has no FDE, so
// "not covered by any FDE" IS "not code" -- no heuristic, no disassembler, and the information is
// already in the file.
//
// SAFETY DIRECTION. Everything here fails towards "this word is code", which is today's behaviour:
// an unexpected pointer encoding, an absent `.eh_frame`, a short read, or a table too large to
// hold all return 0 and leave g_code_n == 0, i.e. no map. The coalescing below only ever GROWS
// intervals, so the map is a SUPERSET of the real functions. The dangerous direction would be to
// exclude a real function -- its x18 sites would then go unrewritten and read a zeroed x18 -- and
// nothing in this function can produce that.
//
// Returns the number of intervals installed (0 == no map).
static int x18_build_code_map(int fd, long off, size_t rlen, unsigned long va) {
  gm_ehdr eh;
  gm_phdr* ph = NULL;
  unsigned char* tab = NULL;
  unsigned char* frames = NULL;
  x18_code_map_clear();
  // RDX18_NOFDE=1 disables the code map entirely, restoring the pre-91802ef whole-image behaviour
  // (every EXECINSTR word treated as an instruction, one unclassifiable word abandoning the image
  // to the racy shared global). Together with RDX18_NODYNSYM this makes all three configurations
  // -- no gate / FDE only / FDE + STT_FUNC -- reachable from ONE binary, which is the only way an
  // A/B on this cannot be confounded by two builds or two machine loads.
  static int nofde = -1;
  if (nofde < 0) nofde = getenv("RDX18_NOFDE") != 0;
  if (nofde) return 0;
  if (fd < 0 || off < 0) return 0;
  if (pread(fd, &eh, sizeof eh, 0) != (ssize_t)sizeof eh) return 0;
  if (memcmp(&eh, "\177ELF", 4) || eh.cls != 2 || eh.data != 1 || eh.e_machine != 183) return 0;
  if (!eh.e_phoff || !eh.e_phnum || eh.e_phnum > 512 || eh.e_phentsize < sizeof(gm_phdr)) return 0;

  size_t phsz = (size_t)eh.e_phnum * eh.e_phentsize;
  ph = malloc(phsz);
  if (!ph) return 0;
  if (pread(fd, ph, phsz, (off_t)eh.e_phoff) != (ssize_t)phsz) goto done;

  const gm_phdr* ehf = NULL;
  for (unsigned i = 0; i < eh.e_phnum; i++) {
    const gm_phdr* p = (const gm_phdr*)((unsigned char*)ph + (size_t)i * eh.e_phentsize);
    if (p->p_type == GM_PT_GNU_EH_FRAME) ehf = p;
  }
  if (!ehf || ehf->p_filesz < 12) goto done;

  // link-time vaddr -> file offset, via the PT_LOAD that contains it
  #define VTOFF(v, out) do {                                                                     \
      long _o = -1;                                                                              \
      for (unsigned _i = 0; _i < eh.e_phnum; _i++) {                                             \
        const gm_phdr* _p = (const gm_phdr*)((unsigned char*)ph + (size_t)_i * eh.e_phentsize);  \
        if (_p->p_type != GM_PT_LOAD || !_p->p_filesz) continue;                                 \
        if ((v) >= _p->p_vaddr && (v) < _p->p_vaddr + _p->p_filesz)                              \
          { _o = (long)(_p->p_offset + ((v) - _p->p_vaddr)); break; }                            \
      }                                                                                          \
      (out) = _o;                                                                                \
    } while (0)

  unsigned char hdr[12];
  if (pread(fd, hdr, sizeof hdr, (off_t)ehf->p_offset) != (ssize_t)sizeof hdr) goto done;
  // Only the encoding gcc and clang actually emit is accepted; anything else -> no map.
  if (hdr[0] != 1 || hdr[1] != 0x1b || hdr[2] != 0x03 || hdr[3] != 0x3b) goto done;
  uint32_t fde_count;
  memcpy(&fde_count, hdr + 8, 4);
  if (!fde_count || fde_count > (1u << 22)) goto done;

  size_t tabsz = (size_t)fde_count * 8;
  tab = malloc(tabsz);
  if (!tab) goto done;
  if (pread(fd, tab, tabsz, (off_t)ehf->p_offset + 12) != (ssize_t)tabsz) goto done;

  // The table is datarel/sdata4 against the start of .eh_frame_hdr. Slurp the whole FDE span once
  // rather than doing one 4-byte pread per function.
  unsigned long hdrv = (unsigned long)ehf->p_vaddr;
  unsigned long fmin = ~0UL, fmax = 0;
  for (uint32_t i = 0; i < fde_count; i++) {
    int32_t d; memcpy(&d, tab + (size_t)i * 8 + 4, 4);
    unsigned long f = hdrv + (long)d;
    if (f < fmin) fmin = f;
    if (f + 16 > fmax) fmax = f + 16;
  }
  if (fmin >= fmax || fmax - fmin > (32UL << 20)) goto done;
  long fminoff; VTOFF(fmin, fminoff);
  if (fminoff < 0) goto done;
  size_t fspan = (size_t)(fmax - fmin);
  frames = malloc(fspan);
  if (!frames) goto done;
  if (pread(fd, frames, fspan, (off_t)fminoff) != (ssize_t)fspan) goto done;

  // Collect (guest_lo, guest_hi) for every FDE that lands inside THIS mapping.
  int n = 0;
  for (uint32_t i = 0; i < fde_count; i++) {
    int32_t dloc, dfde;
    memcpy(&dloc, tab + (size_t)i * 8, 4);
    memcpy(&dfde, tab + (size_t)i * 8 + 4, 4);
    unsigned long loc = hdrv + (long)dloc, fde = hdrv + (long)dfde;
    uint32_t range;
    memcpy(&range, frames + (fde - fmin) + 12, 4);   // length,CIE_id,pc_begin,pc_range
    if (!range) continue;
    long lo_off; VTOFF(loc, lo_off);
    if (lo_off < off || (size_t)(lo_off - off) >= rlen) continue;      // not in this mapping
    if (n == X18_CODE_MAX) { n = 0; break; }                           // too many -> coalesce below
    g_code[n].lo = va + (unsigned long)(lo_off - off);
    g_code[n].hi = g_code[n].lo + range;
    n++;
  }
  // ---- SECOND SOURCE, UNIONED IN: `.dynsym` / `.symtab` STT_FUNC extents -------------------
  //
  // `.eh_frame` ALONE IS NOT ENOUGH, and shipping it alone is what broke GnuTLS the first time.
  // An FDE exists only where the compiler emitted CFI, and hand-written assembly routinely does
  // not. MEASURED in this rootfs, and this is the whole reason for this block:
  //
  //     libgmp.so.10.4.1   196 x18 sites,  41 inside an FDE,  155 in FDE GAPS
  //
  // Those 155 are real code -- the largest gap is `__gmpn_sec_tabselect`, an EXPORTED, NAMED
  // function of NEON assembly (18 `ret`s and 61 x18 references in that one gap). libgmp is fully
  // veneered today, so gating on FDEs alone would take it from 196/196 rewritten to 41/196 and
  // leave 155 real x18 sites reading a register XNU zeroes -- inside GnuTLS's bignum arithmetic.
  // That is precisely the regression that 91802ef shipped and 4223410 reverted: its verification
  // measured libcrypto, and libcrypto is not on the path of the thing that broke.
  //
  // A UNION is the right shape because it can only ever move a word from "skipped as data" to
  // "treated as code" -- the safe direction, the same direction the interval merging below fails
  // in. It cannot re-admit the constant tables: MEASURED, adding this source leaves libcrypto's
  // split at 1141 covered / 1962 uncovered, and libgcrypt's, libgnutls's and libnettle's
  // completely unchanged, because perlasm's tables are not STT_FUNC.
  //
  // `.symtab` is accepted too, when present, for the same reason and at no extra cost. Neither
  // source is trusted ALONE: `.symtab` is absent from every stripped library here and `.dynsym`
  // omits every `static` function, so either on its own would exclude real code. Together with
  // the FDEs they only add coverage.
  //
  // A rootfs-wide sweep (tools/x18extents.c, all 1794 ELFs in disk/) says exactly three images
  // need this: libgmp (155 sites), libmng.so.1.1.0.10 (2890), libXdmcp.so.6.0.0 (3).
  //
  // RDX18_NODYNSYM=1 turns this second source OFF, leaving `.eh_frame` alone. It exists so the
  // regression can be reproduced and the fix demonstrated in ONE binary: with it set, libgmp logs
  // 155 rewriteable sites skipped as data; without it, 0. A gate whose PASS branch cannot be made
  // to fail on demand is not evidence.
  static int nodyn = -1;
  if (nodyn < 0) nodyn = getenv("RDX18_NODYNSYM") != 0;
  if (!nodyn && n && eh.e_shoff && eh.e_shnum && eh.e_shnum <= 4096 &&
      eh.e_shentsize >= sizeof(gm_shdr)) {
    size_t shsz = (size_t)eh.e_shnum * eh.e_shentsize;
    unsigned char* sh = malloc(shsz);
    if (sh && pread(fd, sh, shsz, (off_t)eh.e_shoff) == (ssize_t)shsz) {
      for (unsigned i = 0; i < eh.e_shnum && n; i++) {
        const gm_shdr* s = (const gm_shdr*)(sh + (size_t)i * eh.e_shentsize);
        if (s->sh_type != GM_SHT_DYNSYM && s->sh_type != GM_SHT_SYMTAB) continue;
        if (!s->sh_size || s->sh_entsize < sizeof(gm_sym)) continue;
        if (s->sh_size > (64UL << 20)) continue;                 // refuse a wild size
        size_t nsym = (size_t)(s->sh_size / s->sh_entsize);
        unsigned char* sy = malloc((size_t)s->sh_size);
        if (!sy) continue;
        if (pread(fd, sy, (size_t)s->sh_size, (off_t)s->sh_offset) == (ssize_t)s->sh_size) {
          for (size_t k = 0; k < nsym; k++) {
            const gm_sym* y = (const gm_sym*)(sy + k * (size_t)s->sh_entsize);
            if ((y->st_info & 0xf) != GM_STT_FUNC || !y->st_size || !y->st_value) continue;
            long so; VTOFF((unsigned long)y->st_value, so);
            if (so < off || (size_t)(so - off) >= rlen) continue;   // not in this mapping
            if (n == X18_CODE_MAX) { n = 0; break; }                // -> no map, i.e. all code
            g_code[n].lo = va + (unsigned long)(so - off);
            g_code[n].hi = g_code[n].lo + y->st_size;
            n++;
          }
        }
        free(sy);
      }
    }
    free(sh);
  }

  // This sort is what actually UNIONS the two sources, and it is a qsort rather than the insertion
  // sort this function originally carried. That was fine when the only source was `.eh_frame_hdr`,
  // whose table is already ordered by initial_location, so insertion sort ran linearly. `.dynsym`
  // is ordered by hash bucket, NOT by address, so appending it makes the insertion sort quadratic
  // over up to X18_CODE_MAX (32768) intervals -- work paid at every DSO load, in a process that
  // loads hundreds of them.
  qsort(g_code, (size_t)n, sizeof g_code[0], code_ext_cmp);
  // Merge touching intervals. Merging only ever ADDS bytes to the map, so a wider threshold is
  // strictly more conservative -- it can let a data word through, never exclude a code word.
  int m = 0;
  for (int i = 0; i < n; i++) {
    if (m && g_code[i].lo <= g_code[m - 1].hi) {
      if (g_code[i].hi > g_code[m - 1].hi) g_code[m - 1].hi = g_code[i].hi;
    } else g_code[m++] = g_code[i];
  }
  g_code_n = m;
  #undef VTOFF
done:
  free(frames); free(tab); free(ph);
  return g_code_n;
}

// Rewrite the executable bytes of a freshly staged file mapping. `stg`/`rlen` is the staging
// buffer, `fd`/`off` the file range it was read from, and `va` the guest address it will live at.
// Falls back to the plain (no-x18) rewrite of the whole buffer whenever the section table is
// absent, unreadable, or does not describe this mapping -- the old behaviour, never worse.
// The executable sections of the ELF behind `fd` that fall inside this mapping's file range,
// as byte offsets into the staging buffer. Returns the count, or -1 when the section table is
// absent, unreadable, or does not describe this mapping (caller then keeps the old behaviour).
static int mapped_code_sections(int fd, long off, size_t rlen, struct gm_sect* out, int max) {
  gm_ehdr eh;
  if (fd < 0 || off < 0) return -1;
  if (pread(fd, &eh, sizeof eh, 0) != (ssize_t)sizeof eh) return -1;
  if (memcmp(&eh, "\177ELF", 4) || eh.cls != 2 || eh.data != 1 || eh.e_machine != 183) return -1;
  if (!eh.e_shoff || !eh.e_shnum || eh.e_shentsize < sizeof(gm_shdr)) return -1;
  if (eh.e_shnum > 4096) return -1;                         // refuse to allocate on a wild count

  size_t shsz = (size_t)eh.e_shnum * eh.e_shentsize;
  unsigned char* sh = malloc(shsz);
  if (!sh) return -1;
  if (pread(fd, sh, shsz, (off_t)eh.e_shoff) != (ssize_t)shsz) { free(sh); return -1; }

  // Within one mapping the file offset and the guest address advance together, so a section at
  // file offset sh_offset lands at stg + (sh_offset - off) and runs at va + (sh_offset - off).
  int n = 0;
  for (unsigned i = 0; i < eh.e_shnum; i++) {
    const gm_shdr* s = (const gm_shdr*)(sh + (size_t)i * eh.e_shentsize);
    if (!(s->sh_flags & GM_SHF_ALLOC) || !(s->sh_flags & GM_SHF_EXECINSTR)) continue;
    if (!s->sh_size || s->sh_type == GM_SHT_NOBITS) continue;
    uint64_t lo = s->sh_offset, hi = s->sh_offset + s->sh_size;
    if (hi <= (uint64_t)off || lo >= (uint64_t)off + rlen) continue;    // not in this mapping
    if (lo < (uint64_t)off) lo = (uint64_t)off;
    if (hi > (uint64_t)off + rlen) hi = (uint64_t)off + rlen;
    if (n == max) { free(sh); return -1; }                   // too many: fall back to plain
    out[n].bo  = (size_t)(lo - (uint64_t)off);
    out[n].len = (size_t)(hi - lo) & ~(size_t)3;
    if (out[n].len) n++;
  }
  free(sh);
  return n;
}

static void sects_to_ranges(struct x18_range* r, const struct gm_sect* s, int n,
                            unsigned char* stg, unsigned long va) {
  for (int i = 0; i < n; i++) { r[i].buf = stg + s[i].bo; r[i].n = s[i].len;
                                r[i].va = va + s[i].bo; }
}

// Veneer bytes this mapping's code needs, or 0 if it cannot (or need not) be veneered. Measured
// with va == 0: a veneer's length is address-independent, so this is exact before the image has
// an address, which is what lets the arena be reserved together with the image below.
static size_t mapped_veneer_bytes(unsigned char* stg, const struct gm_sect* s, int n,
                                  int fd, long off, size_t rlen) {
  if (n <= 0) return 0;
  struct x18_range r[GM_MAX_CODE_SECTS];
  sects_to_ranges(r, s, n, stg, 0);
  // The code map has to be built for the SAME va this measures with (0), or measure and rewrite
  // would disagree about which words are sites and the arena would be sized for the wrong set.
  x18_build_code_map(fd, off, rlen, 0);
  size_t bytes = x18_measure_ranges(r, n);
  x18_code_map_clear();
  return bytes;
}

// Rewrite a staged file mapping's code. `arena`/`arena_bytes` is veneer space the caller has
// already reserved adjacent to the image (0 falls back to the shared arena).
//
// elfload.c veneers the main binary and the interpreter, because it has their section tables and
// so knows exactly which bytes are code. Everything ELSE the dynamic linker pulls in at runtime --
// libGL, libX11, Mesa's software rasterizer (6001 x18 sites), libLLVM (13735) -- arrives here
// instead, as an ordinary guest mmap, and used to be rewritten with va == 0, i.e. with NO x18 pass
// at all. Those images were therefore left exposed to macOS zeroing x18 behind the guest's back
// (see KNOWN_BUGS.md): survivable for something like libX11 that barely touches x18, immediately
// fatal for a software rasterizer that spends every cycle in it.
//
// Scoping to SHF_EXECINSTR is not cosmetic -- an R-X segment also holds .rodata, and the x18
// rewrite is aggressive enough (every instruction naming x18, not a handful of exact encodings)
// that a constant which happened to look like one would otherwise be replaced by a branch.
static void rewrite_mapped_code(unsigned char* stg, size_t rlen, int fd, long off,
                                unsigned long va, const struct gm_sect* sect, int nsect,
                                unsigned long arena, size_t arena_bytes) {
  if (nsect <= 0) { gmem_rewrite_exec_insns(stg, rlen); return; }

  // The svc/tpidr rewrite still covers the WHOLE buffer, exactly as before: it matches three
  // precise encodings that the rewrite-site audit showed never occur in .rodata, and keeping it
  // whole-buffer means a mapping whose section table we misread cannot lose a syscall trap.
  gmem_rewrite_exec_insns_va(stg, rlen, 0);

  struct x18_range r[GM_MAX_CODE_SECTS];
  sects_to_ranges(r, sect, nsect, stg, va);
  // `.eh_frame` says which of those EXECINSTR bytes are actually functions. Without it, an aarch64
  // `.text` full of perlasm constant tables gets rewritten as if it were code -- see x18_is_code.
  g_code_skipped_sites = 0;
  int ncode = x18_build_code_map(fd, off, rlen, va);
  x18_neutralize_ranges(r, nsect, arena, arena_bytes);   // all sections succeed, or none do
  x18_code_map_clear();
  if (g_verbose)
    fprintf(stderr, "[gmem] x18: %d exec section(s) scoped in mapping va=%#lx +%#zx; "
                    "%d code interval(s) from .eh_frame + STT_FUNC%s; %lu rewriteable site(s) "
                    "skipped as data\n",
            nsect, va, rlen, ncode, ncode ? "" : " (none -- whole-image fallback kept)",
            g_code_skipped_sites);
}

// ---------------------------------------------------------------------------
// guest mmap / mprotect / munmap
// ---------------------------------------------------------------------------

// Read up to n bytes at `off` into dst. A short read at EOF is normal (last page of a segment);
// the caller has already zeroed the destination. Returns 0 on success, -1 on a real error.
static int pread_all(int fd, void* dst, size_t n, off_t off) {
  size_t done = 0;
  while (done < n) {
    ssize_t r = pread(fd, (char*)dst + done, n - done, off + (off_t)done);
    if (r < 0) { if (errno == EINTR) continue; return -1; }
    if (r == 0) break;                                      // EOF — remainder stays zero
    done += (size_t)r;
  }
  return 0;
}

static void* wx_serve(size_t rlen);        // fault-driven W^X; defined above gmem_mmap()

static long gmem_mmap_locked(unsigned long addr, size_t len, int prot, int flags,
                             int fd, long off) {
  if (len == 0 || len > SIZE_MAX - GPAGE) return -LEINVAL;
  size_t rlen = (size_t)PAGE_UP(len);

  int anon   = (flags & LMAP_ANONYMOUS) != 0 || fd < 0;
  int fixed  = (flags & LMAP_FIXED) != 0;
  int shared = (flags & LMAP_SHARED) != 0;
  int hbase  = (shared ? MAP_SHARED : MAP_PRIVATE) | (anon ? MAP_ANON : 0);
  if (anon) { fd = -1; off = 0; }

  if (fixed && (addr & (GPAGE - 1))) return -LEINVAL;       // MAP_FIXED must be page aligned

  // ---- MAP_FIXED landing on tracked JIT pages -----------------------------
  if (fixed && reg_overlapping((uintptr_t)addr, (uintptr_t)addr + rlen) >= 0) {
    if (prot & PROT_WRITE) {
      // The guest wants to WRITE these pages from its own thread, which runs write-protect(1).
      // JIT can't do that, so hand the span back to the VM as plain RW and stop tracking it.
      void* p = mmap((void*)addr, rlen, PROT_READ | PROT_WRITE,
                     MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
      if (p == MAP_FAILED) return gm_fail();
      reg_untrack((uintptr_t)addr, (uintptr_t)addr + rlen);
      if (fd >= 0 && pread_all(fd, p, len, (off_t)off) != 0) return gm_fail();
      if (g_verbose)
        fprintf(stderr, "[gmem] fixed RW over JIT %#lx +%#zx (untracked)\n", addr, rlen);
      return (long)addr;
    }
    // Read-only / executable: keep the JIT pages and publish through the staging buffer.
    // A pread() straight into JIT memory would be silently dropped by copyout.
    unsigned char* stg = stage_get(rlen);
    if (!stg) return -LENOMEM;
    memset(stg, 0, rlen);
    if (fd >= 0 && pread_all(fd, stg, len, (off_t)off) != 0) return gm_fail();
    if (prot & PROT_EXEC) {
      // MAP_FIXED into an existing reservation: the span is already fixed, so veneers cannot be
      // reserved INSIDE the mapping the way the not-fixed file path does. They can still be put
      // NEXT TO it -- g_vn_hint makes x18_neutralize_ranges call plain_arena_near(), which mmaps
      // a plain arena adjacent to the image and only accepts a placement `b` can reach.
      //
      // THIS PATH IS GLIBC'S. musl's ld.so maps a DSO's text with one not-fixed executable mmap
      // (handled at the bottom of this function, with an adjacent arena reserved in the same
      // gmem_reserve); glibc's ld.so reserves the whole DSO span PROT_NONE and then MAP_FIXEDs
      // each segment into it, so EVERY glibc DSO -- libc.so.6 first among them -- arrives here.
      // Falling back to the shared MAP_JIT arena is not good enough: MAP_JIT is kernel-placed and
      // routinely lands far more than `b`'s +-128 MB away, and the bail is silent in the way that
      // matters --
      //
      //   [gmem] x18: no veneer arena reachable from all 3 range(s) (297 sites, 11 KB);
      //          leaving this image on the old (racy) path
      //
      // "the old (racy) path" being gmem_guest_x18, ONE process-global word shared by every guest
      // thread. That is the entire C runtime of every glibc process -- memcpy, strlen, malloc --
      // computing on a register macOS zeroes ~1500 times a second, and wrong x18 does not fault,
      // it returns plausible wrong bytes.
      struct gm_sect sect[GM_MAX_CODE_SECTS];
      int nsect = mapped_code_sections(fd, off, rlen, sect, GM_MAX_CODE_SECTS);
      unsigned long saved_hint = g_vn_hint;
      g_vn_hint = addr;
      rewrite_mapped_code(stg, rlen, fd, off, addr, sect, nsect, 0, 0);
      g_vn_hint = saved_hint;
      // AND THE SYSCALL FAST PATH, for the same reason this block exists: it is glibc's path, so
      // it is libc.so.6's, so it is where essentially every syscall a real program makes lives.
      // The rewrite above left `brk #0`, and servicing a brk is a cross-thread Mach exception --
      // MEASURED 9443 ns/call against 93 ns for the same syscall natively. fastcall turns the
      // site into `bl <thunk>` and it runs in-thread instead.
      //
      // Patching happens on the STAGING buffer (a pread into JIT memory is dropped by copyout),
      // so the `bl` displacement has to be computed against `addr`, where the code will actually
      // run -- fastcall_patch_va, not fastcall_patch_ex.
      { unsigned long th = fastcall_thunk_once();
        if (th) {
          int sk = 0;
          int pat = fc_stub_patch(stg, rlen, addr, sect, nsect, th, &sk);
          if (g_verbose && (pat || sk))
            fprintf(stderr, "[fastcall] glibc DSO %#lx +%#zx: stubbed=%d skipped=%d\n",
                    addr, rlen, pat, sk);
        } }
    }
    if (gmem_map_code((void*)addr, stg, rlen) != 0) return -LENOMEM;
    if (prot & PROT_EXEC) img_record(addr, rlen, fd);
    if (g_verbose)
      fprintf(stderr, "[gmem] fixed %s over JIT %#lx +%#zx (staged)\n",
              (prot & PROT_EXEC) ? "RX" : "RO", addr, rlen);
    return (long)addr;
  }

  // ---- MAP_FIXED anywhere else: plain host mapping ------------------------
  if (fixed) {
    // If the guest wants it EXECUTABLE, it has to be rewritten first — map it writable, let the
    // contents land, then rewrite and flip to R+X.
    int want_exec = (prot & PROT_EXEC) != 0;
    int mprot = want_exec ? (PROT_READ | PROT_WRITE) : prot;
    void* p = mmap((void*)addr, rlen, mprot, hbase | MAP_FIXED, fd, (off_t)off);
    if (p == MAP_FAILED) return gm_fail();
    if (want_exec) {
      rewrite_runtime_exec(p, rlen, fd, off);
      img_record((unsigned long)(uintptr_t)p, rlen, fd);
    }
    return (long)(uintptr_t)p;
  }

  // ---- anonymous ----------------------------------------------------------
  if (anon) {
    // PROT_NONE reservations MUST be JIT. glibc/musl ld.so reserves the whole DSO span PROT_NONE
    // and then MAP_FIXEDs the executable text into it; if the reservation is not JIT there is no
    // way to make that text executable later (no RWX on Apple Silicon).
    // RWX in one call is a JIT that does NOT do W^X: it writes code and runs it through the
    // same mapping, with no mprotect in between. MAP_JIT is exactly wrong for that -- its
    // writability is a per-thread toggle and the guest thread runs write-protect(1), so the
    // guest's first store faults. PCRE2's regex JIT does this (mmap(0, 0x10000, RWX)), and it
    // is on Qt's startup path, so it is not a corner case.
    //
    // MEASURED, and it corrects the claim this comment used to make: arm64 macOS REJECTS
    // PROT_WRITE|PROT_EXEC at mmap time with EACCES, entitlement or not. Only RW-then-
    // mprotect(RX) works. plain_arena_near() does NOT depend on RWX either -- it carries a
    // g_plain_rwx fallback (gmem.c:516/536/556) that I had overlooked.
    //
    // So there is nothing to hand back that satisfies a simultaneous-W-and-X request. The
    // choice is between failing the call and returning MAP_JIT pages the guest cannot write,
    // and FAILING IS BETTER: a well-behaved JIT treats mmap failure as "no JIT available" and
    // degrades to its interpreter, which is correct and safe. PCRE2's sljit does exactly that,
    // and Qt's regex work then completes. MAP_JIT pages would instead fault on the guest's
    // first store, which is the bug this branch was added to fix.
    //
    // THAT FIX IS NOW BUILT — see the W^X block above gmem_mmap(). It had to be, because the
    // "well-behaved JIT degrades to its interpreter" outcome this branch relied on is exactly
    // what made every web page in this project unloadable: JavaScriptCore takes the degraded
    // path silently and permanently (`useJIT=false`, measured), and an interpreter-only JSC is
    // the ~1.8 pinned cores that never finish microsoft.com. Falling back is correct and safe,
    // and it is also unusably slow, which for a browser is the same as broken.
    //
    // CAVEAT, still true and now MEASURED rather than assumed: code written here is never
    // rewritten, because rewriting is triggered by the guest making memory EXECUTABLE and this
    // memory was born executable. So a guest JIT taking this path keeps raw x18 / svc /
    // mrs tpidr_el0 if it emits any. wx_scan_x18() counts x18 sites in every page at the moment
    // it becomes executable, so the risk is reported as a number instead of being argued about.
    if (anon && (prot & PROT_EXEC) && (prot & PROT_WRITE)) {
      void* p = wx_serve(rlen);
      if (!p) p = MAP_FAILED;
      if (p == MAP_FAILED) {
        if (g_verbose)
          fprintf(stderr, "[gmem] anon RWX %#zx refused by the kernel (%s) -- reporting it to the "
                          "guest so its JIT can fall back to an interpreter\n", rlen,
                  strerror(errno));
        return gm_fail();                     // RDWXNO=1, or even plain RW unavailable
      }
      img_record((unsigned long)(uintptr_t)p, rlen, -1);
      if (g_verbose)
        fprintf(stderr, "[gmem] anon RWX %p +%#zx (plain, not MAP_JIT: guest JIT without W^X)\n",
                p, rlen);
      return (long)(uintptr_t)p;
    }
    if ((prot & PROT_EXEC) || prot == PROT_NONE) {
      void* p = gmem_reserve(rlen);
      if (!p) return -LENOMEM;
      // Anonymous + executable is guest-generated code (a JIT). Record it so a fault there is
      // identifiable as such rather than showing up as an address belonging to nothing.
      if (prot & PROT_EXEC) img_record((unsigned long)(uintptr_t)p, rlen, -1);
      return (long)(uintptr_t)p;
    }
    // A non-FIXED `addr` is a HINT on Linux, and it has to be one here too: mremap's in-place
    // grow is expressed as "map the tail right after the old region, and check you got it".
    // Passing NULL threw the hint away, so there was no way to ask that question at all.
    // Host mmap honours a non-FIXED hint when the range is free and silently places the mapping
    // elsewhere when it is not, which is exactly Linux's rule -- the caller compares.
    void* p = mmap((void*)(uintptr_t)PAGE_DOWN(addr), rlen, prot, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) return gm_fail();
    return (long)(uintptr_t)p;
  }

  // ---- file backed, not fixed ---------------------------------------------
  if (!(prot & PROT_EXEC)) {
    // Read-only, or writable-but-not-executable: the host can map this directly and the guest
    // can read/write it from its own thread. (A JIT reservation would NOT be writable there.)
    void* p = mmap(NULL, rlen, prot, hbase, fd, (off_t)off);
    if (p == MAP_FAILED) return gm_fail();
    return (long)(uintptr_t)p;
  }
  {
    // Executable file mapping — the ld.so "map the DSO with the text segment's prot" case.
    // This is how EVERY runtime DSO arrives, so it is where the x18 veneering has to work.
    //
    // Order matters: the file is staged and measured BEFORE the guest span is reserved, so the
    // veneers can be reserved in the SAME mapping, immediately after the image. A `b` only
    // reaches +-128 MB, and MAP_JIT is kernel-placed (it cannot take MAP_FIXED), so a separately
    // reserved arena lands wherever the kernel likes -- for these DSOs, ~181 MB away, out of
    // range, which made every large image fall back to the racy path.
    unsigned char* stg = stage_get(rlen);
    if (!stg) return -LENOMEM;
    memset(stg, 0, rlen);
    if (pread_all(fd, stg, len, (off_t)off) != 0) return gm_fail();

    struct gm_sect sect[GM_MAX_CODE_SECTS];
    int nsect = mapped_code_sections(fd, off, rlen, sect, GM_MAX_CODE_SECTS);
    size_t vbytes = (nsect > 0) ? mapped_veneer_bytes(stg, sect, nsect, fd, off, rlen) : 0;
    vbytes = (vbytes + 15) & ~(size_t)15;                   // keep the image's own end aligned

    void* p = gmem_reserve(rlen + vbytes);
    if (!p) return -LENOMEM;
    rewrite_mapped_code(stg, rlen, fd, off, (unsigned long)(uintptr_t)p, sect, nsect,
                        vbytes ? (unsigned long)(uintptr_t)p + rlen : 0, vbytes);
    // THE FAST PATH FOR EVERY REAL LIBRARY. The rewrite above left `brk #0` where the guest had
    // `svc #0`, and servicing that brk is a cross-thread Mach exception -- MEASURED at 9443 ns
    // against 93 ns for the same syscall natively, a 101x tax. fastcall turns the site into
    // `bl <thunk>` and the syscall runs in-thread. It was previously only applied in elfload.c,
    // which sees the main binary and the interpreter; THIS is the path every runtime DSO takes,
    // libc.so.6 included, and libc is where essentially every syscall a program makes lives.
    // Patched on the STAGING buffer, so the displacement must be computed against p (the final
    // run address) rather than stg -- hence fastcall_patch_va.
    { unsigned long th = fastcall_thunk_once();
      if (th) {
        int pat = 0, sk = 0, s1;
        if (nsect > 0) {
          for (int i = 0; i < nsect; i++) {
            pat += fastcall_patch_va(stg + sect[i].bo, sect[i].len,
                                     (unsigned long)(uintptr_t)p + sect[i].bo, th, &s1);
            sk += s1;
          }
        } else {
          pat += fastcall_patch_va(stg, rlen, (unsigned long)(uintptr_t)p, th, &s1);
          sk += s1;
        }
        if (g_verbose && (pat || sk))
          fprintf(stderr, "[fastcall] DSO %p +%#zx: patched=%d skipped(x30-live)=%d\n",
                  p, rlen, pat, sk);
      } }
    if (gmem_map_code(p, stg, rlen) != 0) {
      gmem_munmap((unsigned long)(uintptr_t)p, rlen + vbytes);
      return -LENOMEM;
    }
    img_record((unsigned long)(uintptr_t)p, rlen, fd);
    if (g_verbose)
      fprintf(stderr, "[gmem] file RX -> jit %p +%#zx (+%#zx veneers)\n", p, rlen, vbytes);
    return (long)(uintptr_t)p;
  }
}

// ---------------------------------------------------------------------------
// FAULT-DRIVEN W^X EMULATION — what makes JavaScriptCore's JIT exist at all
//
// MEASURED, and this is the whole JavaScript problem: WebKit's JSC asks for its executable memory
// pool with ONE call — anonymous `mmap(0x20008000, PROT_READ|PROT_WRITE|PROT_EXEC)` — arm64 macOS
// refuses plain RWX (EACCES, entitlement or not), the branch below used to report that failure
// honestly, and JSC responded by DISABLING ITS OWN JIT:
//
//     [gmem] anon RWX 0x20008000 refused by the kernel (Permission denied)
//     20936: Got executable pool reservation at (nil)...0x20000000     <- JSC, base == NULL
//        useJIT=false (default: true)                                  <- JSC, having given up
//
// and then ran LLInt-only for the rest of the process's life. Measured cost: a JS arithmetic loop
// ran at 91,478,367 ops/sec with JSC_useJIT=0 and 91,657,880 with JSC_useJIT=1 — 0.2% apart,
// i.e. the flag did nothing because there was never a JIT to turn on. That is what a WebProcess
// pinned at 1.8 cores that never finishes a page load looks like.
//
// The two variables were separated with a guest probe (tests/wl/wxprobe.c) so the fix targets the
// right one:
//     RWX @ 0x20008000 -> EACCES        RW @ 0x20008000 -> ok, 0x11adb4000
//     RWX @ 0x10000    -> EACCES        RW @ 0x10000    -> ok
// **It is the RWX, not the size.** RX and PROT_NONE at the same 512 MB both succeed.
//
// So: serve the request as plain RW and flip protections per page ON DEMAND, which is exactly the
// fix the old comment on the branch below named and declined to build. A page is R+W or R+X, never
// both, and the fault tells us which one the guest wants next:
//     instruction-fetch fault on an R+W page  ->  mprotect R+X   (guest is about to run new code)
//     write fault         on an R+X page      ->  mprotect R+W   (guest is emitting/patching code)
// This is legal on Apple Silicon: only SIMULTANEOUS W and X is refused, and mprotect between the
// two works in both directions (proved by the probe cell "reserve NONE then mprotect RWX/RW").
//
// WHY NOT MAP_JIT, which really does report prot=rwx max=rwx: its writability is a PER-THREAD
// toggle, and `pthread_jit_write_protect_np(0)` makes a thread unable to execute ANY MAP_JIT page.
// In this shim all guest text is MAP_JIT, so a guest thread flipped writable could not execute the
// very memcpy it is running. The toggle cannot be handed to the guest. mprotect can.
//
// THE STATE IS OURS, NOT THE VM's. One bit per page, and we are the only writer, so the handler
// never has to ask mach_vm_region what a page currently is. That also gives every branch below a
// REACHABLE FAIL: a fetch fault on a page we already made executable, or a write fault on a page
// we already made writable, is NOT a protection problem — we decline it, and the caller reports
// and dies exactly as before. A wild pointer therefore cannot be silently retried forever.
//
// SEPARATE LOCK, DELIBERATELY. This runs on the Mach exception-server thread while the faulting
// guest thread is stopped, and that thread may be holding gm_lock (gmem_mprotect memcpy's through
// pages while holding it). Taking gm_lock here would deadlock the process on the first write fault
// that lands during an mmap. Nothing in this block calls back into the gm_lock'd machinery.
// ---------------------------------------------------------------------------
#define GM_WX_MAX 64
// How much of a pool the reserved arena serves, and how big the reservation is. These must be
// sized together: the worst branch is from the far end of the covered span back to the arena's
// base, i.e. COVER + SIZE, and that has to stay inside `b`'s +/-128 MB with room to spare. 64 + 4
// leaves 60 MB of margin. Sizing the arena to mirror the pool (the first attempt) put that worst
// branch at exactly 128 MB and every reservation was rejected.
#define WX_ARENA_COVER  (64UL << 20)
// Sized by EXHAUSTION, not by how much veneer code is needed. Each rewritten pool page consumes a
// whole arena page: a chunk must be flipped R+X before the code page that branches into it goes
// live, and flipping a page that already holds live veneers would have to make it writable first.
// So the arena needs one page per rewritten page, and a browser run rewrites thousands (2258 exec
// flips on microsoft.com). At 4 MB it ran dry after 256 pages and every page after that fell
// through to the hole search, which cannot help mid-pool -- which is exactly how a page 78 MB
// into a pool, comfortably inside `b` range of its own arena, ended up with no arena at all.
#define WX_ARENA_SIZE   (32UL << 20)             // 2048 pages per end, two ends per pool
typedef struct {
  uintptr_t lo, hi;
  unsigned char* x;                  // bit per page: 1 = currently R+X, 0 = currently R+W
  // Veneer arena for THIS pool, reserved next to it the moment the pool is served. Placing it
  // later, per page, is a lottery: by then the pool occupies every nearby address, so mmap without
  // MAP_FIXED lands wherever it likes and `b` (+/-128 MB) usually cannot reach it. That lottery is
  // measurable -- the same jsbench2 run leaves 1 site raw when it wins and 964 when it loses -- and
  // a single raw write to x18 desynchronises every later shadow read, which is the whole bug.
  uintptr_t ar_lo, ar_hi, ar_next;
  // A SECOND arena above the pool's top. A guest JIT allocates from both ends of its pool -- the
  // pages that went unrewritten were every pool's last page (…ffc000, one page below a 512 MB
  // boundary) -- and one arena at the base is 512 MB from those, far outside `b`'s +/-128 MB.
  uintptr_t a2_lo, a2_hi, a2_next;
} gm_wxr;
static gm_wxr g_wxr[GM_WX_MAX];
static int    g_wxr_n;
static pthread_mutex_t g_wxmu = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long g_wx_xflip, g_wx_wflip, g_wx_decl, g_wx_x18, g_wx_scanned, g_wx_raced, g_wx_dirty;

static int wx_off(void) {                       // RDWXNO=1 restores the old "fail the mmap" arm
  static int off = -1;
  if (off < 0) off = getenv("RDWXNO") != 0;
  return off;
}

// Final counts, on EVERY exit, not only under an instrument flag. The reason this is
// unconditional: exec_flips and write_flips are the PRICE of the emulation, and a workload where
// write_flips is a large multiple of exec_flips is THRASHING -- the guest is writing pages it is
// also executing, and each round trip costs a Mach exception. That is the failure mode of this
// whole design, so it must be visible in every run rather than behind a knob nobody sets.
void gmem_wx_report(void) {
  if (!g_wxr_n && !g_wx_xflip && !g_wx_wflip) return;
  fprintf(stderr, "[wx] FINAL pid=%d ranges=%d exec_flips=%llu write_flips=%llu declined=%llu "
                  "raced=%llu x18_sites_in_generated_code=%llu over %llu page(s) [%llu page(s) still dirty]%s\n",
          (int)getpid(), g_wxr_n, g_wx_xflip, g_wx_wflip, g_wx_decl, g_wx_raced, g_wx_x18,
          g_wx_scanned, g_wx_dirty,
          (g_wx_xflip && g_wx_wflip > 4 * g_wx_xflip)
            ? "   >>> THRASH: write flips far exceed exec flips; the guest is writing pages it is "
              "also executing and every round trip costs a Mach exception" : "");
  fflush(stderr);
}

// Serve an anonymous simultaneous-W-and-X request. Returns NULL if even plain RW is unavailable,
// in which case the caller reports the failure to the guest as before.
static void* wx_serve(size_t rlen) {
  if (wx_off()) return NULL;
  size_t npg = rlen / GPAGE;
  unsigned char* bits = calloc(1, npg / 8 + 1);
  if (!bits) return NULL;
  // Allocate the veneer arena and the pool as ONE mapping, arena first, and hand the guest the
  // pool half. Adjacency is then structural: the arena is always exactly WX_ARENA_SIZE below the
  // pool base, so `b` reaches it from every page in the pool's first COVER bytes, always.
  //
  // Searching for a spot next to an already-placed pool does NOT work, and the reason is worth
  // recording: mmap with an unavailable hint and no MAP_FIXED does not look NEAR the hint, it
  // falls back to the kernel's own arena, which is far away. So every retry returns the same
  // distant address and more attempts cannot help -- stepping the hint in 1 MB units over a 64 MB
  // window succeeded in 1 run out of 5, no better than a single try. MAP_FIXED is not an option
  // either: it would silently unmap whatever already lives there.
  uintptr_t ar_lo = 0, ar_hi = 0, a2_lo = 0, a2_hi = 0;
  void* p;
  void* whole = mmap(NULL, (size_t)WX_ARENA_SIZE * 2 + rlen, PROT_READ | PROT_WRITE,
                     MAP_ANON | MAP_PRIVATE, -1, 0);
  if (whole != MAP_FAILED) {
    ar_lo = (uintptr_t)whole;
    ar_hi = ar_lo + (size_t)WX_ARENA_SIZE;
    p = (void*)ar_hi;
    a2_lo = (uintptr_t)p + rlen;                 // immediately above the pool's top page
    a2_hi = a2_lo + (size_t)WX_ARENA_SIZE;
  } else {
    // The combined reservation is only WX_ARENA_SIZE larger, so this should not happen; if it
    // does, serve the pool alone and leave its x18 raw rather than failing the guest's mmap.
    p = mmap(NULL, rlen, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
  }
  if (p == MAP_FAILED) { free(bits); return NULL; }
  pthread_mutex_lock(&g_wxmu);
  if (g_wxr_n >= GM_WX_MAX) {
    pthread_mutex_unlock(&g_wxmu);
    fprintf(stderr, "[wx] TABLE FULL at %d ranges: %p +%#zx gets no W^X emulation, so the guest "
                    "JIT will fault there\n", g_wxr_n, p, rlen);
    free(bits);
    return p;                                   // still better than failing: reads/writes work
  }
  g_wxr[g_wxr_n].lo = (uintptr_t)p;
  g_wxr[g_wxr_n].hi = (uintptr_t)p + rlen;
  g_wxr[g_wxr_n].x  = bits;
  g_wxr[g_wxr_n].ar_lo = g_wxr[g_wxr_n].ar_next = ar_lo;
  g_wxr[g_wxr_n].ar_hi = ar_hi;
  g_wxr[g_wxr_n].a2_lo = g_wxr[g_wxr_n].a2_next = a2_lo;
  g_wxr[g_wxr_n].a2_hi = a2_hi;
  int slot = g_wxr_n++;
  pthread_mutex_unlock(&g_wxmu);
  fprintf(stderr, "[wx] serving guest RWX %p +%#zx as W^X-emulated RW (%zu pages) — this is what "
                  "keeps JSC's useJIT true\n", p, rlen, npg);
  if (g_wxr[slot].ar_lo)
    fprintf(stderr, "[wx] veneer arena %#lx +%#lx reserved next to the pool (covers its first "
                    "%zu MB — pages beyond that stay raw)\n",
            (unsigned long)g_wxr[slot].ar_lo,
            (unsigned long)(g_wxr[slot].ar_hi - g_wxr[slot].ar_lo), (size_t)WX_ARENA_COVER >> 20);
  else
    fprintf(stderr, "[wx] NO veneer arena could be placed within `b` range of %p — guest-JIT x18 "
                    "in this pool stays RAW\n", p);
  return p;
}

// Count x18 references in a page of guest-GENERATED code, AFTER the rewrite pass has run on it.
//
// This used to be a count instead of a fix, on the grounds that JIT output has no symbol table and
// no FDEs, so there is no code/data oracle and an in-place rewrite could plant `brk` over a
// constant pool the way it once did over libcrypto's K256. That objection was inherited from
// LLVM and MEASURED NOT TO APPLY to JavaScriptCore: 64 JSC JIT pages, 65,536 words, 0 invalid
// under llvm-mc, against a negative control where 4096 random bytes invalidate 618/1024 words.
// JSC's ARM64 backend materialises constants with movz/movk rather than emitting literal pools
// (its ARMv7 backend and LLVM do the latter), so its pages are code end to end.
//
// So the scan's meaning has INVERTED, and that is the point of keeping it. It no longer reports
// how much racy x18 we are tolerating; it is the post-condition check on the rewrite, and the
// only thing that can still make it non-zero is a page whose veneer arena was out of `b` range.
// Zero here is now a claim that the pass worked, and it is reachable: it was 167-694 before.
static void wx_scan_x18(uintptr_t pg) {
  const unsigned int* w = (const unsigned int*)pg;
  unsigned n = 0, first_i = 0, first_w = 0; int first_k = 0;
  for (unsigned i = 0; i < GPAGE / 4; i++) {
    unsigned f;
    int k = gmem_x18_classify(w[i], &f);
    if (k != GMEM_X18_NONE) {
      if (!n) { first_i = i; first_w = w[i]; first_k = k; }
      n++;
    }
  }
  g_wx_scanned++;
  if (n) {
    g_wx_x18 += n;
    g_wx_dirty++;      // PAGES, not sites: the printed message is capped at 8 and cannot be counted
    static unsigned long long said;
    if (++said <= 8)
      // Name the actual WORD, not just a count. A surviving x18 site desynchronises every later
      // read of the shadow -- a raw write lands in the hardware register that XNU then zeroes,
      // while rewritten reads see a stale shadow -- so which instruction survived is the whole
      // question, and a bare count cannot answer it.
      fprintf(stderr, "[wx] x18 SURVIVED THE REWRITE: %u site(s) in page %#lx, first at +%#x = "
                      "%#010x (kind %d) (total %llu over %llu pages)\n",
              n, (unsigned long)pg, first_i * 4, first_w, first_k, g_wx_x18, g_wx_scanned);
  }
}

// A fault whose page is ALREADY in the state the fault wants. Two threads can fault on the same
// page before either is served: the first takes g_wxmu and fixes the protection, the second then
// finds the work already done. That second thread is not making a bad access — its instruction
// will now succeed — but the old code declined it, and a declined fault kills the process.
//
// This is not a hypothetical race. Every browser run died here, all with the same signature: a
// 16-byte `str q3, [x0]` inside libc's memcpy called from JavaScriptCore, hitting pool pages 18,
// 19 and 23 while exec flips were working over pages 2-17, and always with exactly declined=1.
// JSC writes to its code pool from several threads, so concurrent faults on one page are normal
// traffic, not an error.
//
// Retrying is safe here in a way it would not be in general: the address is inside a tracked
// region that is one anonymous PROT_READ|PROT_WRITE mapping, so every page in it is backed, and
// the only thing that can fault is our own protection dance. The bound below still keeps the
// original guarantee that a wild jump cannot spin forever — if the SAME address keeps arriving
// with nothing left to fix, the retry is not making progress and we decline as before.
//
// Caller must hold g_wxmu. Returns 1 to resume the thread, 0 to decline.
static int wx_raced(unsigned long addr) {
  static unsigned long last;
  static unsigned n;
  if (addr == last && ++n > 64) return 0;       // not converging: treat as a genuine bad access
  if (addr != last) { last = addr; n = 1; }
  g_wx_raced++;
  return 1;
}

// 1 = flipped a page, resume the faulting thread at the same pc. 0 = not ours; report and die.
int gmem_wx_fault(unsigned long addr, int instruction_fetch) {
  if (wx_off()) return 0;
  uintptr_t pg = PAGE_DOWN(addr);
  int handled = 0, scan = 0;
  pthread_mutex_lock(&g_wxmu);
  for (int i = 0; i < g_wxr_n; i++) {
    if (addr < g_wxr[i].lo || addr >= g_wxr[i].hi) continue;
    size_t ix = (pg - g_wxr[i].lo) / GPAGE;
    unsigned char* b = &g_wxr[i].x[ix / 8];
    int is_x = (*b >> (ix % 8)) & 1;
    if (instruction_fetch) {
      // Already executable and still faulting on FETCH: either another thread just served this
      // same page (see wx_raced below) or it is a wild jump. Both are handled there.
      if (is_x) { handled = wx_raced(addr); if (!handled) g_wx_decl++; break; }
      // THIS is the publication point for guest-GENERATED code, and it is the only one there is.
      // Code born executable is never rewritten by the load-time pass, so until now every x18 in
      // JSC's output stayed raw and XNU zeroed it underneath the guest ~1500 times a second. The
      // exec flip is exactly the moment the page holds finished code and is still writable, so
      // rewrite it here — the same pass, the same veneers, the same arena policy the DSO and
      // MCJIT paths already use. rewrite_runtime_exec() does the whole dance (R+W -> rewrite ->
      // R+X -> icache), so it replaces the bare mprotect this used to do.
      //
      // Re-rewriting on EVERY exec flip is required, not merely tolerated: unlike MCJIT (which is
      // publish-once), JSC writes and re-publishes the same page repeatedly, so each flip can
      // carry new instructions. It is also safe to repeat — a word already turned into
      // `b <veneer>` classifies as NONE, so a second pass over unchanged code is a no-op.
      //
      // Reachability is the one thing that can still fail. A `b` reaches +/-128 MB but JSC's pool
      // is a single 512 MB reservation, so a page in its middle is out of reach of any arena
      // placeable outside the pool. plain_arena_near() simply fails there and the page stays raw
      // — today's behaviour, so no regression — while pages near the pool's base (where JSC
      // actually allocates) get veneered.
      // Tell the arena allocator which mapping encloses this page, so it looks for free memory
      // OUTSIDE the pool instead of probing addresses inside it that are already mapped.
      g_vn_region_lo = g_wxr[i].lo;
      g_vn_region_hi = g_wxr[i].hi;
      // Pick the end that `b` can actually reach from THIS page: low pages use the arena below
      // the pool, high pages the one above it.
      if (g_wxr[i].ar_lo && b_in_range(pg, g_wxr[i].ar_next)) {
        g_vn_pre_next = &g_wxr[i].ar_next; g_vn_pre_hi = g_wxr[i].ar_hi;
      } else if (g_wxr[i].a2_lo && b_in_range(pg, g_wxr[i].a2_next)) {
        g_vn_pre_next = &g_wxr[i].a2_next; g_vn_pre_hi = g_wxr[i].a2_hi;
      }
      int rw_ok = rewrite_runtime_exec((void*)pg, (size_t)GPAGE, -1, 0);
      g_vn_pre_next = NULL; g_vn_pre_hi = 0;
      g_vn_region_lo = g_vn_region_hi = 0;
      if (!rw_ok) { g_wx_decl++; break; }
      *b |= (unsigned char)(1u << (ix % 8));
      g_wx_xflip++; handled = 1; scan = 1;
      // UNCONDITIONAL, and it is the only line that proves the JIT is EXECUTING rather than merely
      // that its mmap succeeded. Serving the reservation is not the claim; a page going executable
      // is. Without this, a run where JSC allocated its pool and then never emitted anything looks
      // exactly like a run where the JIT is working. Rate-limited so a browser cannot flood stderr.
      if (g_wx_xflip == 1 || g_wx_xflip == 100 || (g_wx_xflip % 5000) == 0)
        fprintf(stderr, "[wx] JIT CODE WENT EXECUTABLE: exec flip #%llu at %#lx "
                        "(write flips %llu, declined %llu)\n",
                g_wx_xflip, (unsigned long)pg, g_wx_wflip, g_wx_decl);
    } else {
      // Already writable and still faulting on a WRITE: another thread's write fault on this same
      // page beat us to it, or the access is genuinely bad. wx_raced() separates the two.
      if (!is_x) { handled = wx_raced(addr); if (!handled) g_wx_decl++; break; }
      if (mprotect((void*)pg, (size_t)GPAGE, PROT_READ | PROT_WRITE) != 0) { g_wx_decl++; break; }
      *b &= (unsigned char)~(1u << (ix % 8));
      g_wx_wflip++; handled = 1;
    }
    break;
  }
  pthread_mutex_unlock(&g_wxmu);
  // Scan AFTER unlocking and AFTER the page is executable: reading it needs only PROT_READ, and
  // gmem_x18_classify takes no lock, but keeping the scan out of the critical section keeps the
  // per-fault hold time to the mprotect alone.
  if (scan) wx_scan_x18(pg);
  return handled;
}

void gmem_wx_drop(unsigned long lo, unsigned long hi) {
  if (wx_off()) return;
  pthread_mutex_lock(&g_wxmu);
  for (int i = 0; i < g_wxr_n; i++) {
    // Only a munmap that covers a whole range retires it. A partial unmap inside JSC's pool is
    // decommit, not teardown, and the range must stay tracked or every later fault in it declines.
    if (lo <= g_wxr[i].lo && hi >= g_wxr[i].hi) {
      free(g_wxr[i].x);
      g_wxr[i] = g_wxr[--g_wxr_n];
      i--;
    }
  }
  pthread_mutex_unlock(&g_wxmu);
}

void gmem_wx_stats(unsigned long long* xf, unsigned long long* wf, unsigned long long* decl,
                   unsigned long long* x18, unsigned long long* pages, int* nranges) {
  pthread_mutex_lock(&g_wxmu);
  if (xf) *xf = g_wx_xflip;
  if (wf) *wf = g_wx_wflip;
  if (decl) *decl = g_wx_decl;
  if (x18) *x18 = g_wx_x18;
  if (pages) *pages = g_wx_scanned;
  if (nranges) *nranges = g_wxr_n;
  pthread_mutex_unlock(&g_wxmu);
}

static RDGuestRangeLedger g_export_ranges;
static pthread_once_t g_export_ranges_once=PTHREAD_ONCE_INIT;
static void g_export_ranges_init(void){rdguest_range_init(&g_export_ranges);}
void gmem_guest_ranges_fork_child(void){
  /* The child inherits spans via COW, but not the other thread that may have
   * held this lock at fork. GPU admission remains PID-bound and disabled. */
  rdguest_range_fork_child(&g_export_ranges);
}
int gmem_guest_register_main_stack(void *address,size_t length){
  pthread_once(&g_export_ranges_once,g_export_ranges_init);
  return rdguest_range_map(&g_export_ranges,(uintptr_t)address,length,true)?0:-1;
}
int gmem_guest_export_copyout(void *unused,uint64_t guestAddress,
                              const void *bytes,size_t length){
  (void)unused;
  pthread_once(&g_export_ranges_once,g_export_ranges_init);
  return rdguest_range_copyout(&g_export_ranges,(uintptr_t)guestAddress,
                               bytes,length)?0:-1;
}
long gmem_mmap(unsigned long addr, size_t len, int prot, int flags, int fd, long off) {
  pthread_once(&g_export_ranges_once,g_export_ranges_init);
  gm_lock();
  if((flags & LMAP_FIXED)&&len&&len<=SIZE_MAX-GPAGE&&
     addr<=UINTPTR_MAX-PAGE_UP(len))
    rdguest_range_unmap(&g_export_ranges,(uintptr_t)addr,(size_t)PAGE_UP(len));
  long r = gmem_mmap_locked(addr, len, prot, flags, fd, off);
  if(r>0 && len && len<=SIZE_MAX-GPAGE)
    rdguest_range_map(&g_export_ranges,(uintptr_t)r,(size_t)PAGE_UP(len),
                      (prot & PROT_WRITE)!=0 && !(prot & PROT_EXEC));
  gm_unlock();
  return r;
}

long gmem_mprotect(unsigned long addr, size_t len, int prot) {
  if (len == 0) return 0;
  uintptr_t lo = PAGE_DOWN(addr), hi = PAGE_UP(addr + len);
  if (hi <= lo) return -LEINVAL;

  gm_lock();
  // Tracked JIT pages are already R+X and their writability is a per-thread toggle: a real
  // mprotect() adding PROT_WRITE would fail (no RWX), and one dropping it would break the guest.
  // So JIT pages are normally a no-op success; everything else gets a real mprotect.
  //
  // EXCEPT when the guest asks for writable-and-NOT-executable. Then the region is data, and JIT
  // is exactly wrong: the guest thread runs write-protect(1), so a store there faults. This is
  // not hypothetical — it is how every thread gets its stack. musl's pthread_create maps ~8 MB
  // PROT_NONE and then mprotects it RW, and PROT_NONE anonymous reservations have to be JIT
  // (ld.so reserves a DSO span that way and MAP_FIXEDs text into it later). So hand those pages
  // back to the VM as plain RW, preserving their contents, and stop tracking them.
  long rc = 0;
  if ((prot & PROT_WRITE) && !(prot & PROT_EXEC)) {
    for (;;) {
      int i = reg_overlapping(lo, hi);
      if (i < 0) break;
      uintptr_t rlo = g_reg[i].base, rhi = rlo + g_reg[i].len;
      uintptr_t clo = rlo > lo ? rlo : lo, chi = rhi < hi ? rhi : hi;
      if (chi <= clo) break;
      size_t clen = (size_t)(chi - clo);
      // The JIT mapping is readable, so the bytes can be rescued before it is replaced. They
      // matter when the region held code we published (a text page being made writable for a
      // relocation); for a fresh stack reservation they are just zeros.
      void* save = malloc(clen);
      if (save) memcpy(save, (void*)clo, clen);
      void* np = mmap((void*)clo, clen, PROT_READ | PROT_WRITE,
                      MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
      if (np == MAP_FAILED) { free(save); rc = gm_fail(); break; }
      if (save) { memcpy((void*)clo, save, clen); free(save); }
      reg_untrack(clo, chi);
      if (g_verbose)
        fprintf(stderr, "[gmem] mprotect RW over JIT %#lx +%#zx (untracked)\n",
                (unsigned long)clo, clen);
    }
  }
  uintptr_t p = lo;
  while (p < hi) {
    int i = reg_overlapping(p, hi);
    uintptr_t seg_end = hi;
    if (i >= 0) {
      uintptr_t rlo = g_reg[i].base, rhi = rlo + g_reg[i].len;
      if (p >= rlo) { p = (rhi < hi) ? rhi : hi; continue; }   // inside JIT -> skip (no-op)
      seg_end = (rlo < hi) ? rlo : hi;                          // plain pages up to the JIT region
    }
    if (prot & PROT_EXEC) {
      // The guest is publishing code it generated. Rewrite before it can run.
      rewrite_runtime_exec((void*)p, (size_t)(seg_end - p), -1, 0);
      img_record(p, (unsigned long)(seg_end - p), -1);
    } else if (mprotect((void*)p, (size_t)(seg_end - p), prot) != 0) {
      rc = gm_fail(); break;
    }
    p = seg_end;
  }
  if(rc==0) {
    pthread_once(&g_export_ranges_once,g_export_ranges_init);
    rdguest_range_protect(&g_export_ranges,lo,hi-lo,
                          (prot & PROT_WRITE)!=0 && !(prot & PROT_EXEC));
  }
  gm_unlock();
  return rc;
}

long gmem_munmap(unsigned long addr, size_t len) {
  if (len == 0) return 0;
  uintptr_t lo = PAGE_DOWN(addr), hi = PAGE_UP(addr + len);
  if (hi <= lo) return -LEINVAL;

  gm_lock();
  pthread_once(&g_export_ranges_once,g_export_ranges_init);
  rdguest_range_unmap(&g_export_ranges,lo,hi-lo);
  reg_untrack(lo, hi);                      // drops wholly-covered regions, trims/splits partials
  gm_unlock();
  // A stale W^X range on a recycled address is a silent miscompile waiting to happen: the next
  // guest to be handed this address gets our page bits, which describe the DEAD mapping's
  // protections. JSC's MetaAllocator and LLVM's SectionMemoryManager both recycle slabs.
  gmem_wx_drop(lo, hi);

  if (munmap((void*)lo, (size_t)(hi - lo)) != 0) return gm_fail();
  return 0;
}


// --- where do the traps actually come from? ---------------------------------------------------
// Reports the hottest SITES, not the most numerous ones. A veneer removes a site's cost entirely,
// so the value of converting any site is proportional to its dynamic hit count -- and in a
// compositing loop those are wildly skewed.
// See the contract above gmem_x18_trap_dump_near() in gmem.h. `before`/`after` are counts of
// 4-byte guest words either side of the site, not bytes.
void gmem_x18_trap_dump_near(unsigned long pc, int before, int after) {
  if (!g_trap_n) return;
  unsigned long centre = 0;
  struct x18_trap* c = trap_by(pc, 0);                 // pc is the site itself (enter brk)
  if (c) centre = c->site;
  if (!centre) { c = trap_by(pc, 1); if (c) centre = c->site; }   // pc is a stub word
  if (!centre) { c = trap_by(pc - 4, 1); if (c) centre = c->site; } // pc is a stub's leave brk
  if (!centre) {
    fprintf(stderr, "[x18near] %#lx belongs to no trap record (neither site nor stub)\n", pc);
    return;
  }
  unsigned long lo = centre - (unsigned long)before * 4, hi = centre + (unsigned long)after * 4;
  unsigned long base = 0;
  const char* img = gmem_image_for(centre, &base);
  fprintf(stderr, "[x18near] window %#lx..%#lx around site %#lx (%s+%#lx)\n", lo, hi, centre,
          img ? img : "?", img ? centre - base : centre);
  fprintf(stderr, "[x18near]   %-18s %-9s %-9s %-7s %-4s %-3s %-10s %s\n",
          "site", "guestword", "orig", "regime", "kind", "K", "hits", "note");
  for (unsigned long a = lo; a <= hi; a += 4) {
    struct x18_trap* t = trap_by(a, 0);
    unsigned int gw = 0, vinsn = 0;
    memcpy(&gw, (const void*)a, 4);                    // the faulting thread is stopped
    int vn = veneered_site(a, &vinsn);
    unsigned int orig = t ? t->insn : (vn ? vinsn : gw);
    unsigned f; int k = gmem_x18_classify(orig, &f);
    // NAMING THE REGIME IS THE POINT. x18 written under one mechanism and read under another does
    // not transfer, and the three regimes are not interchangeable locations:
    //   TRAP   -> a trap record; the shadow lives in this thread's x18 home
    //   VENEER -> a veneer record; the shadow lives in the same home, reached inline
    //   RACY   -> NEITHER, and the word still names x18: it ran natively into the real x18, which
    //             macOS zeroes on every exception return, so the write went nowhere at all.
    const char* regime = t ? "TRAP" : (vn ? "VENEER" : (k == GMEM_X18_NONE ? "-" : "RACY"));
    const char* note = "-";
    if (!t && !vn) note = (k == GMEM_X18_NONE) ? "no x18"
                                              : "**NAMES x18, NEITHER TRAPPED NOR VENEERED**";
    else if (t && gw != BRK_IMM(GMEM_BRK_X18_ENTER)) note = "**RECORD EXISTS, BRK GONE FROM TEXT**";
    else if (t && t->hits == 0) note = "trap record ok, NEVER EXECUTED";
    fprintf(stderr, "[x18near]   %-#18lx %08x  %08x  %-7s %-4d %-3u %-10llu %s%s\n",
            a, gw, orig, regime, t ? t->kind : 0, t ? t->K : 0,
            t ? t->hits : 0ULL, a == centre ? "<= FAULT SITE  " : "", note);
  }
  fflush(stderr);
}

void gmem_x18_trap_top(int topn) {
  int idx[64]; int n = 0;
  unsigned long long total = 0, fired = 0;
  for (int i = 0; i < g_trap_n; i++) { total += g_trap[i].hits; if (g_trap[i].hits) fired++; }
  if (!total) return;
  if (topn > 64) topn = 64;
  for (int k = 0; k < topn; k++) {
    int best = -1;
    for (int i = 0; i < g_trap_n; i++) {
      int taken = 0;
      for (int j = 0; j < n; j++) if (idx[j] == i) { taken = 1; break; }
      if (taken || !g_trap[i].hits) continue;
      if (best < 0 || g_trap[i].hits > g_trap[best].hits) best = i;
    }
    if (best < 0) break;
    idx[n++] = best;
  }
  fprintf(stderr, "[trapsite] %llu x18 traps from %llu of %d sites (%.1f%% of sites ever fire)\n",
          total, fired, g_trap_n, 100.0 * (double)fired / (g_trap_n ? g_trap_n : 1));
  unsigned long long cum = 0;
  for (int k = 0; k < n; k++) {
    struct x18_trap* t = &g_trap[idx[k]];
    unsigned long base = 0;
    const char* img = gmem_image_for(t->site, &base);
    cum += t->hits;
    fprintf(stderr, "[trapsite]  %2d. %10llu (%5.2f%%, cum %5.1f%%)  %08x  %s+%#lx\n",
            k + 1, t->hits, 100.0 * (double)t->hits / (double)total,
            100.0 * (double)cum / (double)total, t->insn,
            img ? img : "?", img ? t->site - base : t->site);
  }
  fflush(stderr);
}
