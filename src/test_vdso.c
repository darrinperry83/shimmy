// test_vdso.c — standalone verification of the synthesized Linux aarch64 vDSO.
//
// Builds the image via vdso_build(), then parses it with a completely independent ELF/dynsym
// walker that mirrors what musl's __vdsosym() does (PT_LOAD/PT_DYNAMIC -> DT_HASH/DT_SYMTAB/
// DT_STRTAB/DT_VERSYM/DT_VERDEF -> name+version match), calls the resolved function pointers,
// and benchmarks them against a real trapping syscall.
//
// build & run:
//   clang -arch arm64 -O2 -Wall -o test_vdso test_vdso.c vdso.c
//   codesign --force --sign - --entitlements ent.plist test_vdso
//   ./test_vdso
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>

#include "vdso.h"
#include "gmem.h"

// =============================================================================================
// Local gmem stubs (the real gmem.c is owned by another module and is not linked here).
// Same contract: MAP_JIT reservation + write-protect-toggled copy + icache invalidate.
// =============================================================================================
static void*  g_jit      = 0;
static size_t g_jit_len  = 0;
static int    g_used_jit = 0;

void gmem_init(void) {}

void* gmem_reserve(size_t len) {
  void* p = mmap(0, len, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
  if (p != MAP_FAILED) { g_used_jit = 1; }
  else {
    // No JIT entitlement available: fall back to RW-then-RX so the test can still run.
    p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return 0;
    g_used_jit = 0;
  }
  g_jit = p; g_jit_len = len;
  return p;
}

int gmem_map_code(void* addr, const void* bytes, size_t len) {
  if (g_used_jit) {
    pthread_jit_write_protect_np(0);
    memcpy(addr, bytes, len);
    pthread_jit_write_protect_np(1);
  } else {
    memcpy(addr, bytes, len);
    if (mprotect(addr, len, PROT_READ | PROT_EXEC) != 0) return -1;
  }
  sys_icache_invalidate(addr, len);
  return 0;
}

// unused by this test, present so the header's contract is satisfiable if ever linked
void  gmem_begin_load(void) {}
void  gmem_end_load(void) {}
long  gmem_mmap(unsigned long a, size_t l, int p, int f, int fd, long o)
      { (void)a;(void)l;(void)p;(void)f;(void)fd;(void)o; return -38; }
long  gmem_mprotect(unsigned long a, size_t l, int p) { (void)a;(void)l;(void)p; return -38; }
long  gmem_munmap(unsigned long a, size_t l) { (void)a;(void)l; return -38; }
void  gmem_rewrite_exec_insns(unsigned char* b, size_t l) { (void)b; (void)l; }

// =============================================================================================
// Independent ELF64 walker (deliberately re-declared here, not shared with vdso.c)
// =============================================================================================
typedef struct { unsigned char e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
                 uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags;
                 uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } E_Ehdr;
typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr,
                 p_filesz, p_memsz, p_align; } E_Phdr;
typedef struct { int64_t d_tag; uint64_t d_val; } E_Dyn;
typedef struct { uint32_t st_name; unsigned char st_info, st_other; uint16_t st_shndx;
                 uint64_t st_value, st_size; } E_Sym;
typedef struct { uint16_t vd_version, vd_flags, vd_ndx, vd_cnt; uint32_t vd_hash, vd_aux, vd_next; } E_Verdef;
typedef struct { uint32_t vda_name, vda_next; } E_Verdaux;

#define D_HASH 4
#define D_STRTAB 5
#define D_SYMTAB 6
#define D_STRSZ 10
#define D_SYMENT 11
#define D_SONAME 14
#define D_VERSYM 0x6ffffff0
#define D_VERDEF 0x6ffffffc
#define D_VERDEFNUM 0x6ffffffd

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL: "); printf(__VA_ARGS__); \
                              printf("\n"); fails++; } } while (0)

// exact transcription of musl's checkver()
static int checkver(E_Verdef* def, int vsym, const char* vername, const char* strings) {
  vsym &= 0x7fff;
  for (;;) {
    if (!(def->vd_flags & 1) && (def->vd_ndx & 0x7fff) == vsym) break;
    if (def->vd_next == 0) return 0;
    def = (E_Verdef*)((char*)def + def->vd_next);
  }
  E_Verdaux* aux = (E_Verdaux*)((char*)def + def->vd_aux);
  return !strcmp(vername, strings + aux->vda_name);
}

struct vdso_view {
  const char* base;
  const char* strings;
  E_Sym*      syms;
  uint32_t*   hashtab;
  uint16_t*   versym;
  E_Verdef*   verdef;
  uint64_t    strsz, syment, soname;
  E_Ehdr*     eh;
  uint64_t    span;      // PT_LOAD memsz
};

// mirrors musl __vdsosym()'s parse phase
static int vdso_parse(unsigned long ehdr_addr, struct vdso_view* v) {
  memset(v, 0, sizeof *v);
  E_Ehdr* eh = (E_Ehdr*)(uintptr_t)ehdr_addr;
  v->eh = eh;

  if (memcmp(eh->e_ident, "\x7f" "ELF", 4)) { printf("  FAIL: bad ELF magic\n"); fails++; return -1; }
  CHECK(eh->e_ident[4] == 2, "EI_CLASS != ELFCLASS64 (%u)", eh->e_ident[4]);
  CHECK(eh->e_ident[5] == 1, "EI_DATA != ELFDATA2LSB (%u)", eh->e_ident[5]);
  CHECK(eh->e_type == 3, "e_type != ET_DYN (%u)", eh->e_type);
  CHECK(eh->e_machine == 183, "e_machine != EM_AARCH64 (%u)", eh->e_machine);
  CHECK(eh->e_phentsize == sizeof(E_Phdr), "e_phentsize=%u", eh->e_phentsize);

  E_Phdr* ph = (E_Phdr*)((char*)eh + eh->e_phoff);
  E_Dyn*  dynv = 0;
  uint64_t base = (uint64_t)-1;
  for (int i = 0; i < eh->e_phnum; i++, ph = (E_Phdr*)((char*)ph + eh->e_phentsize)) {
    if (ph->p_type == 1)      { base = (uint64_t)(uintptr_t)eh + ph->p_offset - ph->p_vaddr;
                                v->span = ph->p_memsz; }
    else if (ph->p_type == 2) { dynv = (E_Dyn*)((char*)eh + ph->p_offset); }
  }
  if (!dynv || base == (uint64_t)-1) { printf("  FAIL: no PT_LOAD/PT_DYNAMIC\n"); fails++; return -1; }
  v->base = (const char*)(uintptr_t)base;

  for (int i = 0; dynv[i].d_tag; i++) {
    void* p = (void*)(uintptr_t)(base + dynv[i].d_val);
    switch (dynv[i].d_tag) {
      case D_STRTAB:    v->strings = (const char*)p; break;
      case D_SYMTAB:    v->syms    = (E_Sym*)p;      break;
      case D_HASH:      v->hashtab = (uint32_t*)p;   break;
      case D_VERSYM:    v->versym  = (uint16_t*)p;   break;
      case D_VERDEF:    v->verdef  = (E_Verdef*)p;   break;
      case D_STRSZ:     v->strsz   = dynv[i].d_val;  break;
      case D_SYMENT:    v->syment  = dynv[i].d_val;  break;
      case D_SONAME:    v->soname  = dynv[i].d_val;  break;
    }
  }
  CHECK(v->strings != 0, "no DT_STRTAB");
  CHECK(v->syms    != 0, "no DT_SYMTAB");
  CHECK(v->hashtab != 0, "no DT_HASH");
  CHECK(v->syment == sizeof(E_Sym), "DT_SYMENT=%llu (want 24)", (unsigned long long)v->syment);
  CHECK(v->strsz  != 0, "no DT_STRSZ");
  if (!v->verdef) v->versym = 0;
  return fails ? -1 : 0;
}

static void* vdso_lookup(struct vdso_view* v, const char* vername, const char* name) {
  #define OK_TYPES (1<<2 /*STT_FUNC*/ | 1<<1 /*OBJECT*/ | 1<<10 /*GNU_IFUNC*/)
  #define OK_BINDS (1<<1 /*GLOBAL*/ | 1<<2 /*WEAK*/ | 1<<10 /*GNU_UNIQUE*/)
  for (uint32_t i = 0; i < v->hashtab[1]; i++) {
    if (!((1 << (v->syms[i].st_info & 0xf)) & OK_TYPES)) continue;
    if (!((1 << (v->syms[i].st_info >> 4)) & OK_BINDS)) continue;
    if (!v->syms[i].st_shndx) continue;
    if (strcmp(name, v->strings + v->syms[i].st_name)) continue;
    if (v->versym && !checkver(v->verdef, v->versym[i], vername, v->strings)) continue;
    return (void*)(uintptr_t)(v->base + v->syms[i].st_value);
  }
  return 0;
}

// also exercise the *hashed* lookup path (glibc-style) to prove DT_HASH is well-formed
static uint32_t elf_hash(const char* s) {
  const unsigned char* p = (const unsigned char*)s; uint32_t h = 0, g;
  while (*p) { h = (h << 4) + *p++; g = h & 0xf0000000u; if (g) h ^= g >> 24; h &= ~g; }
  return h;
}
static void* vdso_lookup_hashed(struct vdso_view* v, const char* name) {
  uint32_t nb = v->hashtab[0], nc = v->hashtab[1];
  uint32_t* bucket = v->hashtab + 2, *chain = bucket + nb;
  for (uint32_t i = bucket[elf_hash(name) % nb]; i; i = chain[i]) {
    if (i >= nc) return 0;
    if (!strcmp(name, v->strings + v->syms[i].st_name))
      return (void*)(uintptr_t)(v->base + v->syms[i].st_value);
  }
  return 0;
}

// =============================================================================================
// Guest-side prototypes (Linux ABI)
// =============================================================================================
struct lx_timespec { int64_t tv_sec, tv_nsec; };
struct lx_timeval  { int64_t tv_sec, tv_usec; };

typedef long (*fn_cgt)(long, struct lx_timespec*);
typedef long (*fn_gtod)(struct lx_timeval*, void*);
typedef long (*fn_cgr)(long, struct lx_timespec*);
typedef long (*fn_time)(int64_t*);
typedef long (*fn_cpu)(uint32_t*, uint32_t*, void*);

#define LX_REALTIME 0
#define LX_MONOTONIC 1
#define LX_MONOTONIC_RAW 4
#define LX_REALTIME_COARSE 5
#define LX_MONOTONIC_COARSE 6

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

// The guest runs on a worker thread and JIT write-protection is per-thread, so prove the
// emitted thunks are callable from a thread that never touched pthread_jit_write_protect_np().
static fn_cgt g_cgt;
static struct lx_timespec g_tts;
static void* thread_probe(void* _) {
  (void)_;
  long r = g_cgt(LX_MONOTONIC, &g_tts);
  if (r != 0 || g_tts.tv_sec <= 0 || g_tts.tv_nsec < 0 || g_tts.tv_nsec >= 1000000000)
    return (void*)(intptr_t)-1;
  return (void*)(intptr_t)0;
}

int main(void) {
  printf("== synthesized Linux aarch64 vDSO ==\n\n");

  gmem_init();
  vdso_build();
  unsigned long b = vdso_auxv_base();
  printf("[1] vdso_build()\n");
  printf("  AT_SYSINFO_EHDR = 0x%lx  (%s memory)\n", b, g_used_jit ? "MAP_JIT" : "mmap RX fallback");
  if (!b) { printf("  FAIL: vdso_auxv_base() == 0 (libc would fall back to syscalls)\n"); return 1; }
  CHECK((b & 16383) == 0, "image base is not 16K-aligned");

  // ------------------------------------------------------------------ parse
  printf("\n[2] independent ELF parse (musl __vdsosym() algorithm)\n");
  struct vdso_view v;
  if (vdso_parse(b, &v) != 0) { printf("\nPARSE FAILED\n"); return 1; }
  printf("  e_type=ET_DYN e_machine=EM_AARCH64(183) e_phnum=%u e_shnum=%u\n",
         v.eh->e_phnum, v.eh->e_shnum);
  printf("  PT_LOAD memsz=%llu  PT_DYNAMIC ok\n", (unsigned long long)v.span);
  printf("  DT_HASH  @+0x%-5lx nbucket=%u nchain=%u\n",
         (unsigned long)((const char*)v.hashtab - v.base), v.hashtab[0], v.hashtab[1]);
  printf("  DT_SYMTAB@+0x%-5lx DT_SYMENT=%llu\n",
         (unsigned long)((const char*)v.syms - v.base), (unsigned long long)v.syment);
  printf("  DT_STRTAB@+0x%-5lx DT_STRSZ=%llu  DT_SONAME=\"%s\"\n",
         (unsigned long)(v.strings - v.base), (unsigned long long)v.strsz, v.strings + v.soname);
  printf("  DT_VERSYM@+0x%-5lx DT_VERDEF@+0x%lx\n",
         (unsigned long)((const char*)v.versym - v.base),
         (unsigned long)((const char*)v.verdef - v.base));

  // ------------------------------------------------------------------ symbols
  printf("\n[3] symbol resolution (version \"LINUX_2.6.39\")\n");
  static const char* names[] = { "__kernel_clock_gettime", "__kernel_gettimeofday",
                                 "__kernel_clock_getres",  "__kernel_time",
                                 "__kernel_getcpu" };
  void* addr[5];
  for (int i = 0; i < 5; i++) {
    addr[i] = vdso_lookup(&v, "LINUX_2.6.39", names[i]);
    void* h  = vdso_lookup_hashed(&v, names[i]);
    unsigned long lo = (unsigned long)(uintptr_t)v.base;
    unsigned long hi = lo + (unsigned long)v.span;
    int inside = addr[i] && (unsigned long)(uintptr_t)addr[i] >= lo
                         && (unsigned long)(uintptr_t)addr[i] <  hi;
    printf("  %-24s -> %p  off=+0x%-4lx  in-image=%s  dt_hash=%s\n",
           names[i], addr[i],
           addr[i] ? (unsigned long)((char*)addr[i] - v.base) : 0UL,
           inside ? "yes" : "NO",
           h == addr[i] ? "match" : "MISMATCH");
    CHECK(addr[i] != 0, "%s did not resolve", names[i]);
    CHECK(inside, "%s resolved outside the image", names[i]);
    CHECK(h == addr[i], "%s: DT_HASH chain lookup disagrees with linear scan", names[i]);
  }
  // negative controls
  CHECK(vdso_lookup(&v, "LINUX_2.6.39", "__kernel_nope") == 0, "bogus name resolved");
  CHECK(vdso_lookup(&v, "GLIBC_2.17", "__kernel_clock_gettime") == 0,
        "wrong version string still resolved (verdef check is not working)");
  printf("  negative controls: unknown name and wrong version both reject\n");
  if (fails) { printf("\n%d failure(s) before calling anything\n", fails); return 1; }

  // ------------------------------------------------------------------ calls
  printf("\n[4] calling the emitted thunks\n");
  fn_cgt  cgt  = (fn_cgt)addr[0];
  fn_gtod gtod = (fn_gtod)addr[1];
  fn_cgr  cgr  = (fn_cgr)addr[2];
  fn_time tim  = (fn_time)addr[3];
  fn_cpu  cpu  = (fn_cpu)addr[4];

  struct lx_timespec rt = {0}, mo = {0}, mo2 = {0};
  struct timespec host_rt;
  clock_gettime(CLOCK_REALTIME, &host_rt);
  long r = cgt(LX_REALTIME, &rt);
  CHECK(r == 0, "clock_gettime(REALTIME) returned %ld", r);
  printf("  CLOCK_REALTIME   -> {%lld, %lld}   host {%lld, %ld}  delta=%llds\n",
         (long long)rt.tv_sec, (long long)rt.tv_nsec,
         (long long)host_rt.tv_sec, (long)host_rt.tv_nsec,
         (long long)(rt.tv_sec - host_rt.tv_sec));
  CHECK(rt.tv_nsec >= 0 && rt.tv_nsec < 1000000000, "REALTIME tv_nsec out of range");
  CHECK(llabs((long long)rt.tv_sec - (long long)host_rt.tv_sec) <= 2,
        "REALTIME differs from host by more than 2s");
  CHECK(rt.tv_sec > 1700000000, "REALTIME looks like it is not a real wall clock");

  r = cgt(LX_MONOTONIC, &mo);
  CHECK(r == 0, "clock_gettime(MONOTONIC) returned %ld", r);
  for (volatile int i = 0; i < 200000; i++) { }
  r = cgt(LX_MONOTONIC, &mo2);
  CHECK(r == 0, "clock_gettime(MONOTONIC) #2 returned %ld", r);
  long long d = (mo2.tv_sec - mo.tv_sec) * 1000000000LL + (mo2.tv_nsec - mo.tv_nsec);
  printf("  CLOCK_MONOTONIC  -> {%lld, %lld} then {%lld, %lld}  (+%lld ns, strictly increasing)\n",
         (long long)mo.tv_sec, (long long)mo.tv_nsec,
         (long long)mo2.tv_sec, (long long)mo2.tv_nsec, d);
  CHECK(mo.tv_nsec  >= 0 && mo.tv_nsec  < 1000000000, "MONOTONIC tv_nsec out of range");
  CHECK(mo2.tv_nsec >= 0 && mo2.tv_nsec < 1000000000, "MONOTONIC#2 tv_nsec out of range");
  CHECK(d > 0, "MONOTONIC did not strictly increase (delta %lld)", d);

  static const struct { int id; const char* nm; } more[] = {
    { LX_MONOTONIC_RAW,     "CLOCK_MONOTONIC_RAW"    },
    { LX_REALTIME_COARSE,   "CLOCK_REALTIME_COARSE"  },
    { LX_MONOTONIC_COARSE,  "CLOCK_MONOTONIC_COARSE" },
  };
  for (unsigned i = 0; i < sizeof more / sizeof more[0]; i++) {
    struct lx_timespec t = {0};
    r = cgt(more[i].id, &t);
    CHECK(r == 0, "%s returned %ld", more[i].nm, r);
    CHECK(t.tv_nsec >= 0 && t.tv_nsec < 1000000000, "%s tv_nsec out of range", more[i].nm);
    CHECK(t.tv_sec > 0, "%s tv_sec == 0", more[i].nm);
    printf("  %-22s -> {%lld, %lld}\n", more[i].nm, (long long)t.tv_sec, (long long)t.tv_nsec);
  }
  { struct lx_timespec t = {0};
    r = cgt(999, &t);
    printf("  unknown clock 999      -> %ld (negative => libc falls back to a real syscall)\n", r);
    CHECK(r < 0, "unknown clock did not report an error"); }

  struct lx_timeval tv = {0};
  r = gtod(&tv, 0);
  CHECK(r == 0, "gettimeofday returned %ld", r);
  CHECK(tv.tv_usec >= 0 && tv.tv_usec < 1000000, "tv_usec out of range");
  CHECK(llabs((long long)tv.tv_sec - (long long)host_rt.tv_sec) <= 2, "gettimeofday off host");
  printf("  gettimeofday           -> {%lld, %lld}\n", (long long)tv.tv_sec, (long long)tv.tv_usec);

  struct lx_timespec res = {0};
  r = cgr(LX_MONOTONIC, &res);
  CHECK(r == 0 && res.tv_sec == 0 && res.tv_nsec == 1, "clock_getres(MONOTONIC) r=%ld {%lld,%lld}",
        r, (long long)res.tv_sec, (long long)res.tv_nsec);
  printf("  clock_getres(MONO)     -> {%lld, %lld}\n", (long long)res.tv_sec, (long long)res.tv_nsec);
  { struct lx_timespec cres = {0};
    r = cgr(LX_REALTIME_COARSE, &cres);
    CHECK(r == 0 && cres.tv_nsec == 1000000, "clock_getres(REALTIME_COARSE) wrong");
    printf("  clock_getres(COARSE)   -> {%lld, %lld}\n",
           (long long)cres.tv_sec, (long long)cres.tv_nsec); }

  int64_t tt = 0;
  r = tim(&tt);
  CHECK(r == tt, "time(): return %ld != stored %lld", r, (long long)tt);
  CHECK(llabs((long long)tt - (long long)host_rt.tv_sec) <= 2, "time() off host");
  printf("  time                   -> %lld (ret %ld)\n", (long long)tt, r);
  r = tim(0);                                  // NULL arg must be safe
  CHECK(r > 1700000000, "time(NULL) returned %ld", r);

  uint32_t c = 0xdead, n = 0xbeef;
  r = cpu(&c, &n, 0);
  CHECK(r == 0 && c == 0 && n == 0, "getcpu r=%ld cpu=%u node=%u", r, c, n);
  printf("  getcpu                 -> cpu=%u node=%u (ret %ld)\n", c, n, r);
  r = cpu(0, 0, 0);
  CHECK(r == 0, "getcpu(NULL,NULL) returned %ld", r);

  // ------------------------------------------------------------------ ABI edge cases
  printf("\n[5] ABI: garbage x18 on entry, and calling from a worker thread\n");
  {
    // Linux code may use x18 freely; Darwin reserves it. Enter the thunk with it poisoned.
    struct lx_timespec t = {0};
    uint64_t poison = 0xDEADBEEFCAFEF00DULL, seen = 0;
    long rr;
    __asm__ volatile(
        "mov  x9,  x18\n"          // stash Darwin's x18
        "mov  x18, %[poison]\n"    // poison it, exactly as a Linux caller might leave it
        "mov  x0,  %[clk]\n"
        "mov  x1,  %[ts]\n"
        "blr  %[fn]\n"
        "mov  %[out], x0\n"
        "mov  %[seen], x18\n"      // did the thunk give it back?
        "mov  x18, x9\n"           // restore Darwin's x18
        : [out] "=&r"(rr), [seen] "=&r"(seen)
        : [fn] "r"(cgt), [clk] "r"((long)LX_MONOTONIC), [ts] "r"(&t), [poison] "r"(poison)
        : "x0", "x1", "x9", "x30", "memory");
    printf("  entered with x18=0x%llx -> ret %ld, ts={%lld,%lld}, x18 on return=0x%llx (%s)\n",
           (unsigned long long)poison, rr, (long long)t.tv_sec, (long long)t.tv_nsec,
           (unsigned long long)seen, seen == poison ? "preserved" : "changed by the OS");
    CHECK(rr == 0, "call with poisoned x18 returned %ld", rr);
    CHECK(t.tv_sec > 0 && t.tv_nsec >= 0 && t.tv_nsec < 1000000000,
          "call with poisoned x18 produced a bad timespec");
    if (seen != poison) printf("  note: x18 was not preserved end-to-end (OS-owned register)\n");
  }
  {
    pthread_t th;
    g_cgt = cgt;
    if (pthread_create(&th, 0, thread_probe, 0) == 0) {
      void* ret = 0; pthread_join(th, &ret);
      long ok = (long)(intptr_t)ret;
      printf("  worker thread call     -> %s (ts={%lld,%lld})\n",
             ok == 0 ? "ok, JIT text executable there too" : "FAILED",
             (long long)g_tts.tv_sec, (long long)g_tts.tv_nsec);
      CHECK(ok == 0, "vDSO thunk did not work on a worker thread");
    } else { CHECK(0, "pthread_create failed"); }
  }

  // ------------------------------------------------------------------ benchmark
  printf("\n[6] benchmark (trap-free proof)\n");
  const int N = 300000;
  struct lx_timespec sink; struct timespec hsink;
  for (int i = 0; i < 20000; i++) cgt(LX_MONOTONIC, &sink);   // warm

  uint64_t t0 = now_ns();
  for (int i = 0; i < N; i++) cgt(LX_MONOTONIC, &sink);
  uint64_t t1 = now_ns();
  double vdso_ns = (double)(t1 - t0) / N;

  t0 = now_ns();
  for (int i = 0; i < N; i++) clock_gettime(CLOCK_MONOTONIC, &hsink);
  t1 = now_ns();
  double host_ns = (double)(t1 - t0) / N;

  const int M = 60000;
  t0 = now_ns();
  for (int i = 0; i < M; i++) { volatile pid_t p = getppid(); (void)p; }
  t1 = now_ns();
  double sc_ns = (double)(t1 - t0) / M;

  printf("  vDSO __kernel_clock_gettime : %8.2f ns/call\n", vdso_ns);
  printf("  host clock_gettime(3)       : %8.2f ns/call\n", host_ns);
  printf("  real trapping syscall       : %8.2f ns/call  (getppid)\n", sc_ns);
  printf("  vs syscall                  : %8.1fx faster\n", sc_ns / vdso_ns);
  printf("  vs ~10us Mach-exception trap: %8.1fx faster\n", 10000.0 / vdso_ns);
  CHECK(vdso_ns < 200.0, "vDSO call costs %.1f ns/call - that smells like a trap", vdso_ns);
  CHECK(vdso_ns < sc_ns, "vDSO is not faster than a real syscall");

  // ------------------------------------------------------------------ dump for external tools
  const char* dump = getenv("VDSO_DUMP");
  if (dump) {
    FILE* f = fopen(dump, "wb");
    if (f) { fwrite((void*)(uintptr_t)b, 1, (size_t)v.span, f); fclose(f);
             printf("\n  image dumped to %s (%llu bytes)\n", dump, (unsigned long long)v.span); }
  }

  printf("\n%s (%d failure%s)\n", fails ? "*** FAILED ***" : "ALL CHECKS PASSED",
         fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
