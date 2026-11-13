// bootstrap.c — the SysV aarch64 Linux initial process stack (argc/argv/envp/auxv) + guest entry.
//
// The guest is an unmodified Linux binary: at its entry point it expects EXACTLY what the Linux
// kernel's `create_elf_tables()` leaves behind, with SP pointing at `argc`. Every field here is
// load-bearing — a wrong AT_PHDR/AT_PHNUM makes musl's self-relocation walk garbage, a wrong
// AT_RANDOM makes the stack-guard init read unmapped memory, and a misaligned SP makes the first
// `stp x29,x30,[sp,#-N]!` fault (aarch64 requires SP 16-byte aligned at all times).
//
// ============================ STACK LAYOUT (built top-down) ============================
//
//   stack+stksz  ->  +-------------------------------------------+  high addresses
//                    | (up to 15 bytes dropped to 16-align top)  |
//   at_random    ->  | 16 random bytes            <- AT_RANDOM   |
//                    +-------------------------------------------+
//                    | envp[envc-1] string  "\0"                  |   pushed first ...
//                    | ...                                        |
//   envpp[0]     ->  | envp[0] string       "\0"                  |   ... so env[0] ends up lowest
//                    +-------------------------------------------+
//                    | argv[argc-1] string  "\0"                  |
//                    | ...                                        |
//   argvp[0]     ->  | argv[0] string       "\0"  <- AT_EXECFN    |
//                    +-------------------------------------------+
//                    | (0..15 bytes of pad from 16-aligning down) |
//   RETURNED SP  ->  | argc                        (8 bytes)      |  <-- 16-byte aligned
//                    | argv[0] ptr                                |
//                    | ...                                        |
//                    | argv[argc-1] ptr                           |
//                    | NULL                        (argv end)     |
//                    | envp[0] ptr                                |
//                    | ...                                        |
//                    | envp[envc-1] ptr                           |
//                    | NULL                        (envp end)     |
//                    | auxv[0].a_type, auxv[0].a_val              |
//                    | ...                                        |
//                    | AT_NULL, 0                  (auxv end)     |
//                    +-------------------------------------------+
//   stack        ->  | unused (the guest's own stack grows down   |  low addresses
//                    |  from the returned SP into this space)     |
//                    +-------------------------------------------+
//
// The string block is laid out so the final ascending order is
//   argv[0] < argv[1] < ... < env[0] < ... < env[envc-1] < at_random
// which is what Linux itself produces (things like setproctitle()/`ps` assume argv strings are
// contiguous and ascending).
//
// Note the vector base is rounded DOWN to 16, so there may be a small gap between the last auxv
// word and the start of the string block. That is harmless — nothing addresses that gap.
// =======================================================================================
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "bootstrap.h"
#include "vdso.h"

// ---- Linux aarch64 auxv types (asm-generic/auxvec.h) ----
#define AT_NULL          0
#define AT_IGNORE        1
#define AT_PHDR          3
#define AT_PHENT         4
#define AT_PHNUM         5
#define AT_PAGESZ        6
#define AT_BASE          7
#define AT_FLAGS         8
#define AT_ENTRY         9
#define AT_UID          11
#define AT_EUID         12
#define AT_GID          13
#define AT_EGID         14
#define AT_HWCAP        16
#define AT_CLKTCK       17
#define AT_SECURE       23
#define AT_RANDOM       25
#define AT_HWCAP2       26
#define AT_EXECFN       31
#define AT_SYSINFO_EHDR 33

// ---- AT_HWCAP: which optional ISA extensions we tell the guest it may use --------------------
//
// This was 0 ("advertise no optional ISA extensions"), which is the safe default but is NOT free:
// guest code that RUNTIME-DISPATCHES on HWCAP then takes the no-extensions path on hardware that
// has the extensions. The concrete consumer is gcc's `-moutline-atomics`, which Debian enables by
// default: every atomic in every Debian .so goes through a libgcc helper that tests a byte
// initialised from `getauxval(AT_HWCAP) & HWCAP_ATOMICS`, and with 0 it takes an ldxr/stxr retry
// loop instead of one LSE instruction. (Verified statically: libQt6Core.so.6.4.2 carries 17 such
// helper pairs, each `adrp/ldrb/cbz` then either `cas*` or the LL/SC loop.)
//
// WHAT MAY BE CLAIMED HERE IS EXACTLY WHAT THE HOST CPU EXECUTES NATIVELY AND THE SHIM NEVER SEES.
// A HWCAP bit is a promise that an INSTRUCTION will not fault. FP/ASIMD/LSE are plain arithmetic
// the M-series runs at EL0 with no trap, so claiming them costs the shim nothing.
//
// DELIBERATELY NOT CLAIMED, and why -- this list is the safety argument, not a TODO:
//   HWCAP_CPUID (1<<11)  the guest would read MIDR_EL1/ID_AA64* with `mrs`. Linux EMULATES those
//                        at EL0; macOS does not, so the guest would take SIGILL far from the cause.
//   AES/PMULL/SHA1/SHA2/SHA512/SHA3 (1<<3..1<<6, 1<<17, 1<<21)
//                        the instructions exist on this CPU, but claiming them re-routes OpenSSL
//                        onto a different AES implementation, and libcrypto ALREADY miscomputes
//                        ~0.2% of AES operations under this shim (KNOWN_BUGS). Changing which
//                        implementation runs would confound that open bug with this change. Not a
//                        judgement that it is unsafe -- a refusal to entangle two variables.
//   SVE (1<<22)          this hardware has none.
// RDHWCAP=<hex> overrides the value, so the A/B is one env var and the win can be shown going away.
#define HWCAP_FP       (1UL << 0)
#define HWCAP_ASIMD    (1UL << 1)
#define HWCAP_ATOMICS  (1UL << 8)     // LSE: cas/ldadd/swp/... (hw.optional.arm.FEAT_LSE = 1)
#define GUEST_HWCAP  (HWCAP_FP | HWCAP_ASIMD | HWCAP_ATOMICS)

#define GUEST_PAGESZ 16384   // the whole shim assumes a 16K guest page (Apple Silicon native)
#define GUEST_UID      501
#define GUEST_GID       20
#define GUEST_CLKTCK   100

#define INLINE_PTRS  64      // argv+envp pointers we can index without touching the heap
#define MAX_AUX_PAIRS 32     // capacity of the local auxv scratch array (we emit ~19)

// RDHWCAP=<hex> overrides AT_HWCAP for A/B (RDHWCAP=0 restores the previous behaviour exactly).
static unsigned long guest_hwcap(void) {
  const char* v = getenv("RDHWCAP");
  if (v && *v) return (unsigned long)strtoull(v, NULL, 16);
  return GUEST_HWCAP;
}

// Push `len` bytes onto the descending stack cursor. Returns the address written to, or NULL if
// the region would underflow below `lo`.
static void* push_bytes(uintptr_t* sp, uintptr_t lo, const void* src, size_t len) {
  if ((size_t)(*sp - lo) < len) return NULL;   // *sp >= lo is an invariant, so this can't wrap
  *sp -= len;
  memcpy((void*)*sp, src, len);
  return (void*)*sp;
}

static inline uintptr_t align_down16(uintptr_t x) { return x & ~(uintptr_t)15; }

void* build_stack(void* stack, size_t stksz, int argc, char** argv, char** envp,
                  const struct prog_info* p) {
  if (!stack || !p || stksz < 4096) return NULL;
  if (argc < 0) argc = 0;
  if (!argv) argc = 0;

  int envc = 0;
  if (envp) while (envp[envc]) envc++;

  const uintptr_t lo = (uintptr_t)stack;
  uintptr_t sp = align_down16(lo + stksz);     // cursor, walks DOWN from the top of the region

  // Pointer index for the strings we copy. Small counts stay on our own stack; anything larger
  // (deep `find -exec` style argv, or a fat environment) heap-allocates and is freed before return.
  void*  inline_idx[INLINE_PTRS];
  void** idx  = inline_idx;
  void** heap = NULL;
  size_t nidx = (size_t)argc + (size_t)envc;
  if (nidx > INLINE_PTRS) {
    heap = (void**)malloc(nidx * sizeof(void*));
    if (!heap) return NULL;
    idx = heap;
  }
  void** argvp = idx;             // [0, argc)
  void** envpp = idx + argc;      // [0, envc)

  void* result = NULL;

  // ---- 1. AT_RANDOM: 16 bytes at the very top ----
  unsigned char rnd[16];
  arc4random_buf(rnd, sizeof rnd);
  void* at_random = push_bytes(&sp, lo, rnd, sizeof rnd);
  if (!at_random) goto done;

  // ---- 2. envp strings (pushed last-to-first so envp[0] lands lowest) ----
  for (int i = envc - 1; i >= 0; i--) {
    const char* s = envp[i];
    size_t len = strlen(s) + 1;
    void* a = push_bytes(&sp, lo, s, len);
    if (!a) goto done;
    envpp[i] = a;
  }

  // ---- 3. argv strings (same ordering trick) ----
  for (int i = argc - 1; i >= 0; i--) {
    const char* s = argv[i] ? argv[i] : "";
    size_t len = strlen(s) + 1;
    void* a = push_bytes(&sp, lo, s, len);
    if (!a) goto done;
    argvp[i] = a;
  }

  // AT_EXECFN is argv[0]; with no argv at all, hand libc a real empty string rather than NULL.
  void* at_execfn;
  if (argc > 0) {
    at_execfn = argvp[0];
  } else {
    at_execfn = push_bytes(&sp, lo, "", 1);
    if (!at_execfn) goto done;
  }

  // ---- 4. the auxiliary vector ----
  // Emitted into a local array and COUNTED, so the word budget below can never drift out of sync
  // with the list (a hand-maintained pair-count #define is exactly how this file rots).
  unsigned long aux[2 * MAX_AUX_PAIRS];
  size_t na = 0;
#define AUX(t, v)                                                     \
  do {                                                                \
    if (na + 2 > sizeof aux / sizeof aux[0]) goto done;               \
    aux[na++] = (unsigned long)(t);                                   \
    aux[na++] = (unsigned long)(v);                                   \
  } while (0)

  AUX(AT_PHDR,   p->phdr);                    // program headers of the MAIN binary, in memory
  AUX(AT_PHENT,  p->phent);
  AUX(AT_PHNUM,  p->phnum);
  AUX(AT_PAGESZ, GUEST_PAGESZ);               // 16K, matching gmem's mapping granularity
  AUX(AT_BASE,   p->interp_base);             // 0 when static: "there is no interpreter"
  AUX(AT_FLAGS,  0);
  AUX(AT_ENTRY,  p->entry);                   // ALWAYS the main program entry, even though we
                                              // branch to the interpreter first — the interpreter
                                              // reads this to find the program it must start.
  AUX(AT_UID,    GUEST_UID);
  AUX(AT_EUID,   GUEST_UID);
  AUX(AT_GID,    GUEST_GID);
  AUX(AT_EGID,   GUEST_GID);
  AUX(AT_HWCAP,  guest_hwcap());              // see GUEST_HWCAP above: only what runs untrapped
  AUX(AT_CLKTCK, GUEST_CLKTCK);
  AUX(AT_SECURE, 0);
  AUX(AT_RANDOM, (unsigned long)at_random);
  AUX(AT_HWCAP2, 0);
  AUX(AT_EXECFN, (unsigned long)at_execfn);
  AUX(AT_SYSINFO_EHDR, vdso_auxv_base());     // 0 is fine: libc falls back to real syscalls
  AUX(AT_NULL,   0);                          // MUST be last
#undef AUX

  // ---- 5. the integer vector, 16-byte aligned ----
  //   [argc][argv0..argvN][NULL][envp0..envpM][NULL][auxv pairs][AT_NULL,0]
  size_t nwords = 1 + (size_t)argc + 1 + (size_t)envc + 1 + na;
  if (nwords > (SIZE_MAX / 8)) goto done;
  size_t vbytes = nwords * 8;
  if ((size_t)(sp - lo) < vbytes) goto done;
  uintptr_t vbase = align_down16(sp - vbytes);
  if (vbase < lo) goto done;

  unsigned long* v = (unsigned long*)vbase;
  size_t n = 0;
  v[n++] = (unsigned long)argc;
  for (int i = 0; i < argc; i++) v[n++] = (unsigned long)argvp[i];
  v[n++] = 0;                                          // end of argv
  for (int i = 0; i < envc; i++) v[n++] = (unsigned long)envpp[i];
  v[n++] = 0;                                          // end of envp
  memcpy(&v[n], aux, na * sizeof(unsigned long));
  n += na;
  if (n != nwords) goto done;                          // pure paranoia; can only fire on an edit

  result = (void*)vbase;                               // 16-aligned by construction

done:
  free(heap);
  return result;
}

unsigned long entry_point(const struct prog_info* p) {
  if (!p) return 0;
  return p->interp_entry ? p->interp_entry : p->entry;
}

// Hand control to the guest. The Linux kernel enters a fresh process with SP set, x29/x30 zero and
// the general registers effectively cleared (x0 may carry an rtld_fini pointer; 0 means "none"),
// so we mirror that instead of leaking host register values into libc's startup.
//
// Constraint note: `entry` and `sp` are pinned to x16/x17 (the intra-procedure scratch registers).
// They must live somewhere the zeroing sequence does NOT touch — if the compiler were free to pick
// x0..x8 for either operand we would clear the branch target or the stack pointer value before
// using it. x18 is the reserved macOS platform register and is deliberately left alone.
__attribute__((noreturn)) void enter_guest(unsigned long entry, void* sp) {
  register unsigned long r_entry __asm__("x16") = entry;
  register unsigned long r_sp    __asm__("x17") = (unsigned long)sp;
  __asm__ __volatile__(
      "mov  sp, %1\n\t"           // switch to the guest stack (points at argc)
      "mov  x0,  #0\n\t"
      "mov  x1,  #0\n\t"
      "mov  x2,  #0\n\t"
      "mov  x3,  #0\n\t"
      "mov  x4,  #0\n\t"
      "mov  x5,  #0\n\t"
      "mov  x6,  #0\n\t"
      "mov  x7,  #0\n\t"
      "mov  x8,  #0\n\t"
      "mov  x29, #0\n\t"          // no host frame chain for the guest to unwind into
      "mov  x30, #0\n\t"          // returning from _start is undefined; make it fault loudly
      "br   %0\n\t"
      :
      : "r"(r_entry), "r"(r_sp)
      : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "memory");
  __builtin_unreachable();
}

// Resume a guest thread with a full register file (see bootstrap.h). x9 is the cursor while the
// file is being reloaded, so it and x16 are restored last / not at all respectively.
__attribute__((noreturn)) void enter_guest_regs(const unsigned long* r) {
  __asm__ __volatile__(
      "mov  x9, %0\n\t"
      "ldr  x10, [x9, #248]\n\t"        // r[31] = sp
      "mov  sp, x10\n\t"
      "ldr  x16, [x9, #256]\n\t"        // r[32] = pc  (kept in IP0, deliberately not restored)
      "ldp  x0,  x1,  [x9, #0]\n\t"
      "ldp  x2,  x3,  [x9, #16]\n\t"
      "ldp  x4,  x5,  [x9, #32]\n\t"
      "ldp  x6,  x7,  [x9, #48]\n\t"
      "ldr  x8,       [x9, #64]\n\t"
      "ldp  x11, x12, [x9, #88]\n\t"
      "ldp  x13, x14, [x9, #104]\n\t"
      "ldr  x15,      [x9, #120]\n\t"
      "ldr  x17,      [x9, #136]\n\t"
      "ldp  x18, x19, [x9, #144]\n\t"
      "ldp  x20, x21, [x9, #160]\n\t"
      "ldp  x22, x23, [x9, #176]\n\t"
      "ldp  x24, x25, [x9, #192]\n\t"
      "ldp  x26, x27, [x9, #208]\n\t"
      "ldp  x28, x29, [x9, #224]\n\t"
      "ldr  x30,      [x9, #240]\n\t"
      "ldr  x10,      [x9, #80]\n\t"    // x10 after we are done using it as a scratch
      "ldr  x9,       [x9, #72]\n\t"    // x9 last: it is the cursor
      "br   x16\n\t"
      :
      : "r"(r)
      : "memory");
  __builtin_unreachable();
}
