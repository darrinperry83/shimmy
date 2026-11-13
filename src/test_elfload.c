// test_elfload.c — standalone harness for elfload.c.
//
// gmem.c is written in parallel, so this file carries a SELF-CONTAINED STUB of the gmem API
// (plain mmap + memcpy, plus a faithful copy of the documented instruction rewrite) so the ELF
// parsing/layout logic can be verified on its own. Build:
//
//     clang -arch arm64 -O2 -Wall test_elfload.c elfload.c -o test_elfload && ./test_elfload
//
// Link against the real gmem.c instead with -DUSE_REAL_GMEM (needs JIT entitlements to run).
#include "elfload.h"
#include "gmem.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// =================================================================================================
// gmem stub
// =================================================================================================
#ifndef USE_REAL_GMEM

#define STUB_MAX_REGIONS 64
static struct { unsigned long base; size_t len; } g_regions[STUB_MAX_REGIONS];
static int  g_nregions;
static int  g_load_depth;         // gmem_begin_load/gmem_end_load nesting
static int  g_load_brackets;      // completed brackets
static int  g_map_code_calls;
static int  g_rewrite_calls;

void gmem_init(void) { g_nregions = 0; }

void* gmem_reserve(size_t len) {
  // The real one hands back MAP_JIT (kernel-placed, never MAP_FIXED, always above 4GB because the
  // low 4GB is __PAGEZERO). Plain anonymous RW is an adequate stand-in for layout testing.
  void* p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (p == MAP_FAILED) return NULL;
  if (g_nregions < STUB_MAX_REGIONS) {
    g_regions[g_nregions].base = (unsigned long)p;
    g_regions[g_nregions].len  = len;
    g_nregions++;
  }
  return p;
}

static int stub_in_region(unsigned long a, size_t len) {
  for (int i = 0; i < g_nregions; i++)
    if (a >= g_regions[i].base && a + len <= g_regions[i].base + g_regions[i].len) return 1;
  return 0;
}

int gmem_map_code(void* addr, const void* bytes, size_t len) {
  if (!stub_in_region((unsigned long)addr, len)) {
    fprintf(stderr, "[stub] gmem_map_code(%p, %zu) outside every tracked region\n", addr, len);
    return -1;
  }
  if (!g_load_depth)
    fprintf(stderr, "[stub] WARNING: gmem_map_code outside a begin/end_load bracket\n");
  memcpy(addr, bytes, len);
  g_map_code_calls++;
  return 0;
}

long gmem_mmap(unsigned long addr, size_t len, int prot, int flags, int fd, long off) {
  int hf = MAP_PRIVATE;                                   // LINUX flag values in, macOS out
  if (flags & 0x01) hf = MAP_SHARED;
  if (flags & 0x10) hf |= MAP_FIXED;
  if (flags & 0x20) { hf |= MAP_ANON; fd = -1; off = 0; }
  void* p = mmap((void*)addr, len, prot, hf, fd, off);
  if (p == MAP_FAILED) return -errno;
  return (long)p;
}

long gmem_mprotect(unsigned long addr, size_t len, int prot) {
  return mprotect((void*)addr, len, prot) ? -errno : 0;
}

long gmem_munmap(unsigned long addr, size_t len) {
  return munmap((void*)addr, len) ? -errno : 0;
}

void gmem_begin_load(void) { g_load_depth++; }
void gmem_end_load(void)   { if (--g_load_depth == 0) g_load_brackets++; }

// The x18 pass lives in the real gmem.c; here it only has to exist. Reporting every encoding as
// "no x18" keeps the loader on its normal path so this test still exercises the mapping logic.
unsigned long gmem_guest_x18;
int gmem_x18_gpr_fields(unsigned int insn) { (void)insn; return 0; }
int gmem_x18_classify(unsigned int insn, unsigned* fields) {
  (void)insn; if (fields) *fields = 0; return GMEM_X18_NONE;
}
void gmem_rewrite_exec_insns_va(unsigned char* buf, size_t len, unsigned long va) {
  (void)va; gmem_rewrite_exec_insns(buf, len);
}

// The documented rewrite, applied to the plain staging buffer before it reaches JIT memory.
void gmem_rewrite_exec_insns(unsigned char* buf, size_t len) {
  g_rewrite_calls++;
  uint32_t* w = (uint32_t*)buf;
  for (size_t i = 0; i + 4 <= len; i += 4, w++) {
    uint32_t insn = *w;
    if (insn == 0xD4000001u) { *w = 0xD4200000u; continue; }              // svc #0 -> brk #0
    if ((insn & ~0x1Fu) == 0xD51BD040u)                                   // msr tpidr_el0, xN
      { *w = 0xD4200000u | ((0x100u + (insn & 0x1F)) << 5); continue; }
    if ((insn & ~0x1Fu) == 0xD53BD040u)                                   // mrs xN, tpidr_el0
      { *w = 0xD4200000u | ((0x200u + (insn & 0x1F)) << 5); continue; }
  }
}
#endif // !USE_REAL_GMEM

// =================================================================================================
// test scaffolding
// =================================================================================================
static int  g_fail;
static int  g_checks;
static long g_total_svc_rewritten;   // across every real binary loaded

#define CHECK(cond, ...)                                                            \
  do {                                                                              \
    g_checks++;                                                                     \
    if (!(cond)) { g_fail++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); }\
  } while (0)

#define CHECK_EQ(a, b, what)                                                        \
  do {                                                                              \
    unsigned long _a = (unsigned long)(a), _b = (unsigned long)(b);                 \
    g_checks++;                                                                     \
    if (_a != _b) { g_fail++; printf("  FAIL: %s: got %#lx want %#lx\n", what, _a, _b); } \
  } while (0)

typedef struct {
  uint8_t  e_ident[16];
  uint16_t e_type, e_machine;
  uint32_t e_version;
  uint64_t e_entry, e_phoff, e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Ehdr;
typedef struct {
  uint32_t p_type, p_flags;
  uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Phdr;

#define PT_LOAD 1
#define PT_INTERP 3
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define GPAGE 16384UL
#define PDOWN(x) ((unsigned long)(x) & ~(GPAGE - 1))
#define PUP(x) PDOWN((unsigned long)(x) + GPAGE - 1)

static unsigned char* slurp(const char* path, size_t* n) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return NULL;
  struct stat st;
  if (fstat(fd, &st)) { close(fd); return NULL; }
  unsigned char* b = malloc((size_t)st.st_size);
  if (!b) { close(fd); return NULL; }
  ssize_t got = read(fd, b, (size_t)st.st_size);
  close(fd);
  if (got != st.st_size) { free(b); return NULL; }
  *n = (size_t)st.st_size;
  return b;
}

static const Phdr* ph_at(const unsigned char* f, int i) {
  const Ehdr* e = (const Ehdr*)f;
  return (const Phdr*)(f + e->e_phoff + (size_t)i * e->e_phentsize);
}

// =================================================================================================
// real-binary test
// =================================================================================================
static void test_real(const char* path, const char* want_interp) {
  printf("== %s\n", path);
  size_t fsz = 0;
  unsigned char* f = slurp(path, &fsz);
  if (!f) { printf("  SKIP (not present)\n"); return; }
  const Ehdr* e = (const Ehdr*)f;

  struct loaded L;
  memset(&L, 0xAB, sizeof L);
#ifndef USE_REAL_GMEM
  int brackets_before = g_load_brackets;
#endif
  int r = elf_load(path, &L);
  CHECK(r == 0, "elf_load returned %d", r);
  if (r != 0) { free(f); return; }
#ifndef USE_REAL_GMEM
  CHECK_EQ(g_load_brackets, brackets_before + 1, "exactly one begin/end_load bracket");
  CHECK_EQ(g_load_depth, 0, "load bracket balanced");
#endif

  printf("  base=%#lx entry=%#lx phdr=%#lx phnum=%d phent=%d interp='%s'\n",
         L.base, L.entry, L.phdr, L.phnum, L.phent, L.interp);

  CHECK(L.base >= 0x100000000UL, "base %#lx must be above the 4GB __PAGEZERO", L.base);
  CHECK_EQ(L.entry, L.base + e->e_entry, "entry == base + e_entry");
  CHECK_EQ(L.phnum, e->e_phnum, "phnum");
  CHECK_EQ(L.phent, e->e_phentsize, "phent");

  // Independent AT_PHDR computation: the PT_LOAD whose file range contains e_phoff.
  unsigned long want_phdr = 0, minv = ~0UL;
  for (int i = 0; i < e->e_phnum; i++) {
    const Phdr* p = ph_at(f, i);
    if (p->p_type == PT_LOAD && p->p_vaddr < minv) minv = p->p_vaddr;
  }
  for (int i = 0; i < e->e_phnum; i++) {
    const Phdr* p = ph_at(f, i);
    if (p->p_type != PT_LOAD) continue;
    if (e->e_phoff >= p->p_offset &&
        e->e_phoff + (uint64_t)e->e_phnum * e->e_phentsize <= p->p_offset + p->p_filesz) {
      want_phdr = L.base + p->p_vaddr + (e->e_phoff - p->p_offset);
      break;
    }
  }
  CHECK_EQ(L.phdr, want_phdr, "phdr == base + p_vaddr + (e_phoff - p_offset)");

  // The phdrs must really be readable at that address and match the file byte for byte.
  CHECK(memcmp((void*)L.phdr, f + e->e_phoff, (size_t)e->e_phnum * e->e_phentsize) == 0,
        "program headers in guest memory match the file");

  if (want_interp) CHECK(strcmp(L.interp, want_interp) == 0,
                         "interp '%s' != '%s'", L.interp, want_interp);

  // Segment content: every PT_LOAD is contiguous inside the reserved span, data segments match the
  // file exactly, exec segments match the file except where a trap was planted, and .bss is zero.
  unsigned long span_lo = ~0UL, span_hi = 0;
  long svc_seen = 0, brk_seen = 0, msr_seen = 0, mrs_seen = 0, stray_svc = 0, data_diff = 0,
       bss_nonzero = 0, gap_bad = 0;
  for (int i = 0; i < e->e_phnum; i++) {
    const Phdr* p = ph_at(f, i);
    if (p->p_type != PT_LOAD || p->p_memsz == 0) continue;
    unsigned long va = L.base + p->p_vaddr;
    if (PDOWN(va) < span_lo) span_lo = PDOWN(va);
    if (PUP(va + p->p_memsz) > span_hi) span_hi = PUP(va + p->p_memsz);
    const unsigned char* src = f + p->p_offset;
    const unsigned char* dst = (const unsigned char*)va;

    if (p->p_flags & PF_X) {
      for (uint64_t o = 0; o + 4 <= p->p_filesz; o += 4) {
        if ((va + o) & 3) break;                        // not instruction-aligned; skip
        uint32_t in, got;
        memcpy(&in, src + o, 4);
        memcpy(&got, dst + o, 4);
        if (in == 0xD4000001u) {
          svc_seen++;
          if (got == 0xD4200000u) brk_seen++;
          else printf("  FAIL: svc#0 at +%#llx not rewritten (got %#x)\n",
                      (unsigned long long)o, got);
        } else if ((in & ~0x1Fu) == 0xD51BD040u) {
          msr_seen++;
          if ((got & 0xFFE0001Fu) != 0xD4200000u) printf("  FAIL: msr tpidr at +%#llx not rewritten\n",
                                                         (unsigned long long)o);
        } else if ((in & ~0x1Fu) == 0xD53BD040u) {
          mrs_seen++;
          if ((got & 0xFFE0001Fu) != 0xD4200000u) printf("  FAIL: mrs tpidr at +%#llx not rewritten\n",
                                                         (unsigned long long)o);
        } else if (got != in) {
          data_diff++;
        }
        if (got == 0xD4000001u) stray_svc++;             // no svc may survive into guest memory
      }
    } else {
      if (p->p_filesz && memcmp(dst, src, (size_t)p->p_filesz) != 0) data_diff++;
    }
    for (uint64_t o = p->p_filesz; o < p->p_memsz; o++)
      if (dst[o]) { bss_nonzero++; break; }
    // the sub-page prefix before p_vaddr must hold the file bytes Linux would have mapped there
    unsigned long gap = va - PDOWN(va);
    if (gap && p->p_offset >= gap &&
        memcmp((void*)PDOWN(va), f + p->p_offset - gap, gap) != 0) gap_bad++;
  }
  CHECK_EQ(gap_bad, 0, "sub-page prefix before p_vaddr holds the file bytes");
  CHECK_EQ(data_diff, 0, "segment bytes match the file");
  CHECK_EQ(bss_nonzero, 0, "the memsz-filesz tail (.bss) is zero");
  CHECK_EQ(stray_svc, 0, "no svc #0 survives in guest memory");
  CHECK(svc_seen == 0 || brk_seen == svc_seen, "%ld of %ld svc #0 rewritten to brk #0",
        brk_seen, svc_seen);
  CHECK(span_hi - span_lo <= 0x40000000UL, "image span is contiguous and sane");
  g_total_svc_rewritten += brk_seen;
  printf("  span=[%#lx,%#lx) %.1f KB · svc#0=%ld (all -> brk#0) msr tpidr=%ld mrs tpidr=%ld\n",
         span_lo, span_hi, (double)(span_hi - span_lo) / 1024.0, svc_seen, msr_seen, mrs_seen);
  free(f);
}

// =================================================================================================
// synthetic ELFs — exact control over the layout math
// =================================================================================================
#define SYN_TEXT_VA 0x0000
#define SYN_DATA_VA 0x30000          // 3 * 64K: distinct pages, like a real linker
#define SYN_NPH     3
#define SYN_TEXTOFF (sizeof(Ehdr) + SYN_NPH * sizeof(Phdr) + 8)   // deliberately unaligned start
#define SYN_INTERP  "/lib/ld-musl-aarch64.so.1"

// Builds a 2-PT_LOAD image: RX text (with an svc/msr/mrs to rewrite) + RW data with a .bss tail.
static unsigned char* synth(int etype, unsigned long vbase, int with_interp, size_t* out_len) {
  size_t interp_off = SYN_TEXTOFF;
  size_t interp_len = with_interp ? sizeof(SYN_INTERP) : 0;
  size_t text_off   = ((interp_off + interp_len) + 15) & ~15UL;
  size_t text_len   = 64;
  size_t data_off   = text_off + text_len;
  size_t data_len   = 100;
  size_t total      = data_off + data_len;

  unsigned char* b = calloc(1, total);
  Ehdr* e = (Ehdr*)b;
  memcpy(e->e_ident, "\177ELF\2\1\1", 7);
  e->e_type = (uint16_t)etype;
  e->e_machine = 183;
  e->e_version = 1;
  e->e_entry = vbase + SYN_TEXT_VA + text_off + 8;     // 3rd instruction of .text
  e->e_phoff = sizeof(Ehdr);
  e->e_ehsize = sizeof(Ehdr);
  e->e_phentsize = sizeof(Phdr);
  e->e_phnum = SYN_NPH;

  Phdr* ph = (Phdr*)(b + sizeof(Ehdr));
  ph[0].p_type = PT_LOAD; ph[0].p_flags = PF_R | PF_X;
  ph[0].p_offset = 0; ph[0].p_vaddr = ph[0].p_paddr = vbase + SYN_TEXT_VA;
  ph[0].p_filesz = ph[0].p_memsz = text_off + text_len;
  ph[0].p_align = 0x10000;

  ph[1].p_type = PT_LOAD; ph[1].p_flags = PF_R | PF_W;
  ph[1].p_offset = data_off; ph[1].p_vaddr = ph[1].p_paddr = vbase + SYN_DATA_VA + data_off;
  ph[1].p_filesz = data_len; ph[1].p_memsz = data_len + 40000;      // .bss tail spanning a page
  ph[1].p_align = 0x10000;

  ph[2].p_type = with_interp ? PT_INTERP : 0 /* PT_NULL */;
  ph[2].p_offset = interp_off; ph[2].p_vaddr = ph[2].p_paddr = vbase + interp_off;
  ph[2].p_filesz = ph[2].p_memsz = interp_len;
  ph[2].p_align = 1;

  if (with_interp) memcpy(b + interp_off, SYN_INTERP, interp_len);

  uint32_t text[16] = {
    0xD503201Fu,               // nop
    0xD53BD053u,               // mrs x19, tpidr_el0   -> brk #(0x200+19)
    0xD4000001u,               // svc #0               -> brk #0
    0xD51BD042u,               // msr tpidr_el0, x2    -> brk #(0x100+2)
    0xAA0003E1u,               // mov x1, x0
    0xD65F03C0u,               // ret
  };
  memcpy(b + text_off, text, sizeof text);
  for (size_t i = 0; i < data_len; i++) b[data_off + i] = (unsigned char)(0x40 + i);
  *out_len = total;
  return b;
}

static char* write_tmp(const unsigned char* b, size_t n, char* buf, size_t bufsz) {
  snprintf(buf, bufsz, "./elfload_test_XXXXXX");
  int fd = mkstemp(buf);
  if (fd < 0) { perror("mkstemp"); return NULL; }
  if (write(fd, b, n) != (ssize_t)n) { perror("write"); close(fd); return NULL; }
  close(fd);
  return buf;
}

static void test_synth_dyn(void) {
  printf("== synthetic ET_DYN (PIE, 2 PT_LOAD, .bss tail, PT_INTERP)\n");
  size_t n = 0;
  unsigned char* b = synth(3 /*ET_DYN*/, 0, 1, &n);
  char path[64];
  if (!write_tmp(b, n, path, sizeof path)) { free(b); g_fail++; return; }

  struct loaded L;
  memset(&L, 0xAB, sizeof L);
  int r = elf_load(path, &L);
  unlink(path);
  CHECK(r == 0, "elf_load returned %d", r);
  if (r != 0) { free(b); return; }

  const Ehdr* e = (const Ehdr*)b;
  const Phdr* ph = (const Phdr*)(b + sizeof(Ehdr));
  printf("  base=%#lx entry=%#lx phdr=%#lx phnum=%d phent=%d interp='%s'\n",
         L.base, L.entry, L.phdr, L.phnum, L.phent, L.interp);

  CHECK(L.base >= 0x100000000UL, "base above __PAGEZERO");
  CHECK_EQ(L.entry, L.base + e->e_entry, "entry");
  CHECK_EQ(L.phdr, L.base + ph[0].p_vaddr + (e->e_phoff - ph[0].p_offset), "phdr");
  CHECK_EQ(L.phdr, L.base + sizeof(Ehdr), "phdr lands on the header copy in guest memory");
  CHECK_EQ(L.phnum, SYN_NPH, "phnum");
  CHECK_EQ(L.phent, sizeof(Phdr), "phent");
  CHECK(strcmp(L.interp, SYN_INTERP) == 0, "interp '%s'", L.interp);

  // exec segment: traps planted, everything else byte-identical
  const uint32_t* t = (const uint32_t*)(L.base + ph[0].p_vaddr + (unsigned long)
                                        (((sizeof(Ehdr) + SYN_NPH * sizeof(Phdr) + 8) +
                                          sizeof(SYN_INTERP) + 15) & ~15UL));
  CHECK_EQ(t[0], 0xD503201Fu, "nop untouched");
  CHECK_EQ(t[1], 0xD4200000u | (0x213u << 5), "mrs x19,tpidr_el0 -> brk #0x213");
  CHECK_EQ(t[2], 0xD4200000u, "svc #0 -> brk #0");
  CHECK_EQ(t[3], 0xD4200000u | (0x102u << 5), "msr tpidr_el0,x2 -> brk #0x102");
  CHECK_EQ(t[4], 0xAA0003E1u, "mov x1,x0 untouched");
  CHECK_EQ(t[5], 0xD65F03C0u, "ret untouched");

  // ELF header bytes are inside the exec segment and must survive verbatim
  CHECK(memcmp((void*)(L.base + ph[0].p_vaddr), b, sizeof(Ehdr)) == 0, "ELF header copied verbatim");

  // data segment: file bytes then a zero .bss tail, contiguous with the text at the right offset
  const unsigned char* d = (const unsigned char*)(L.base + ph[1].p_vaddr);
  CHECK(memcmp(d, b + ph[1].p_offset, (size_t)ph[1].p_filesz) == 0, "data segment bytes");
  int zero = 1;
  for (uint64_t i = ph[1].p_filesz; i < ph[1].p_memsz; i++) if (d[i]) { zero = 0; break; }
  CHECK(zero, ".bss tail (%llu bytes) zeroed",
        (unsigned long long)(ph[1].p_memsz - ph[1].p_filesz));
  *(volatile unsigned char*)d = 0x5A;             // data pages must be writable
  CHECK_EQ(*(volatile unsigned char*)d, 0x5A, "data segment is writable");
  free(b);
}

static void test_synth_exec_low(void) {
  printf("== synthetic ET_EXEC at 0x400000 (must be refused: __PAGEZERO)\n");
  size_t n = 0;
  unsigned char* b = synth(2 /*ET_EXEC*/, 0x400000, 0, &n);
  char path[64];
  if (!write_tmp(b, n, path, sizeof path)) { free(b); g_fail++; return; }
  struct loaded L;
  int r = elf_load(path, &L);
  unlink(path);
  CHECK(r != 0, "elf_load must fail for a sub-4GB ET_EXEC (returned %d)", r);
  free(b);
}

static void test_synth_exec_high(void) {
  printf("== synthetic ET_EXEC at 0x400000000 (16GB)\n");
  size_t n = 0;
  unsigned char* b = synth(2 /*ET_EXEC*/, 0x400000000UL, 0, &n);
  char path[64];
  if (!write_tmp(b, n, path, sizeof path)) { free(b); g_fail++; return; }
  struct loaded L;
  memset(&L, 0xAB, sizeof L);
  int r = elf_load(path, &L);
  unlink(path);
  if (r == 0) {
    CHECK_EQ(L.base, 0, "ET_EXEC honoured p_vaddr: bias must be 0");
    CHECK_EQ(L.entry, ((const Ehdr*)b)->e_entry, "ET_EXEC entry is absolute");
    printf("  loaded at its link address (kernel happened to place the JIT reservation there)\n");
  } else {
    printf("  refused (expected on most runs): MAP_JIT is kernel-placed, so a fixed base cannot\n"
           "  be honoured for executable text. Non-PIE ET_EXEC is unsupported by design.\n");
    g_checks++;   // counted as an accepted outcome, not a failure
  }
  free(b);
}

static void test_bad_inputs(void) {
  printf("== error paths\n");
  struct loaded L;
  CHECK(elf_load("/no/such/file/anywhere", &L) != 0, "missing file must fail");
  CHECK(elf_load("/Users/dperry/linux-shim/ARCHITECTURE.md", &L) != 0, "non-ELF must fail");

  char path[64];
  // wrong machine
  size_t n = 0;
  unsigned char* b = synth(3, 0, 0, &n);
  ((Ehdr*)b)->e_machine = 62;                       // EM_X86_64
  if (write_tmp(b, n, path, sizeof path)) {
    CHECK(elf_load(path, &L) != 0, "wrong e_machine must fail");
    unlink(path);
  }
  free(b);
  // 32-bit class
  b = synth(3, 0, 0, &n);
  ((Ehdr*)b)->e_ident[4] = 1;                       // ELFCLASS32
  if (write_tmp(b, n, path, sizeof path)) {
    CHECK(elf_load(path, &L) != 0, "ELFCLASS32 must fail");
    unlink(path);
  }
  free(b);
  // no PT_LOAD
  b = synth(3, 0, 0, &n);
  { Phdr* ph = (Phdr*)(b + sizeof(Ehdr)); ph[0].p_type = 0; ph[1].p_type = 0; }
  if (write_tmp(b, n, path, sizeof path)) {
    CHECK(elf_load(path, &L) != 0, "no PT_LOAD must fail");
    unlink(path);
  }
  free(b);
  // truncated file
  b = synth(3, 0, 0, &n);
  { unsigned char* small = malloc(24); memcpy(small, b, 24);
    if (write_tmp(small, 24, path, sizeof path)) {
      CHECK(elf_load(path, &L) != 0, "truncated ELF must fail");
      unlink(path);
    }
    free(small); }
  free(b);
}

int main(int argc, char** argv) {
  gmem_init();

  const char* root = argc > 1 ? argv[1] : "/Users/dperry/linux-shim/rootfs";
  char p[1024];

  printf("--- synthetic ---\n");
  test_synth_dyn();
  test_synth_exec_low();
  test_synth_exec_high();
  test_bad_inputs();

  printf("\n--- real aarch64 Linux binaries ---\n");
  snprintf(p, sizeof p, "%s/bin/busybox", root);
  test_real(p, "/lib/ld-musl-aarch64.so.1");
  snprintf(p, sizeof p, "%s/lib/ld-musl-aarch64.so.1", root);
  test_real(p, "");                                  // the interpreter itself has no PT_INTERP
  snprintf(p, sizeof p, "%s/usr/bin/xeyes", root);
  test_real(p, "/lib/ld-musl-aarch64.so.1");
  snprintf(p, sizeof p, "%s/usr/bin/glxgears", root);
  test_real(p, "/lib/ld-musl-aarch64.so.1");
  snprintf(p, sizeof p, "%s/usr/lib/libX11.so.6", root);
  test_real(p, NULL);

  CHECK(g_total_svc_rewritten > 0,
        "no real binary contributed an svc #0 rewrite (rootfs missing?) — got %ld",
        g_total_svc_rewritten);

#ifndef USE_REAL_GMEM
  printf("\nstub: %d gmem_map_code calls, %d rewrite calls, %d load brackets\n",
         g_map_code_calls, g_rewrite_calls, g_load_brackets);
#endif
  printf("\n%d checks, %d failures — %s\n", g_checks, g_fail, g_fail ? "FAILED" : "ALL PASS");
  return g_fail ? 1 : 0;
}
