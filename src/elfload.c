// elfload.c — aarch64 Linux ELF loader (PIE + dynamic) mapping through gmem.
//
// Loads an ET_DYN (normal case: Alpine/musl PIE) or ET_EXEC aarch64 ELF into THIS process's
// address space. The whole PT_LOAD span is reserved ONCE as a single MAP_JIT region so the image
// is contiguous (the guest's dynamic linker walks base+p_vaddr and expects no holes); each
// PT_LOAD is then placed at base + (p_vaddr - min_vaddr):
//
//   * executable segments  -> staged in a plain heap buffer, run through gmem_rewrite_exec_insns()
//                             (svc/msr tpidr/mrs tpidr -> brk), then copied into the JIT region
//                             with gmem_map_code().  A pread CANNOT write JIT memory, and JIT
//                             memory cannot be written without the per-thread APRR toggle, so the
//                             staging buffer is mandatory, not an optimization.
//   * everything else      -> plain RW anonymous pages remapped over the JIT span with MAP_FIXED
//                             via gmem_mmap() (Linux flag values), then memcpy'd.
//
// The low 4GB of an arm64 macOS process is __PAGEZERO and is permanently unmappable, so we never
// request a fixed low address: the kernel picks the base and ET_DYN images take whatever bias it
// gave us.  See the ET_EXEC note in elf_load() for the non-relocatable case.
#include "elfload.h"
#include "gmem.h"
#include "fastcall.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// ---- minimal ELF64 little-endian definitions (macOS has no <elf.h>) --------------------------
typedef struct {
  uint8_t  e_ident[16];
  uint16_t e_type, e_machine;
  uint32_t e_version;
  uint64_t e_entry, e_phoff, e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

typedef struct {
  uint32_t p_type, p_flags;
  uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Elf64_Phdr;

#define ET_EXEC       2
#define ET_DYN        3
#define EM_AARCH64  183
#define ELFCLASS64    2
#define ELFDATA2LSB   1
#define EV_CURRENT    1

#define PT_LOAD       1
#define PT_INTERP     3
#define PT_PHDR       6

#define PF_X          1
#define PF_W          2
#define PF_R          4

// ---- guest page geometry ---------------------------------------------------------------------
#define GPAGE      16384UL
#define PDOWN(x)   ((unsigned long)(x) & ~(GPAGE - 1))
#define PUP(x)     PDOWN((unsigned long)(x) + GPAGE - 1)
#define FOUR_GB    0x100000000UL

// gmem_mmap() takes LINUX flag values, not macOS ones.
#define L_MAP_PRIVATE 0x02
#define L_MAP_FIXED   0x10
#define L_MAP_ANON    0x20
#define L_PROT_READ   1
#define L_PROT_WRITE  2

#define MAX_LOADS 32

typedef struct {
  uint32_t sh_name, sh_type;
  uint64_t sh_flags, sh_addr, sh_offset, sh_size;
  uint32_t sh_link, sh_info;
  uint64_t sh_addralign, sh_entsize;
} Elf64_Shdr;

#define SHF_ALLOC       0x2
#define SHF_EXECINSTR   0x4

#define MAX_CODE_SECTS  32

struct seg { const Elf64_Phdr* ph; };

// The executable PT_LOAD of a two-segment image (musl's ld.so, most Alpine DSOs) carries .rodata
// INSIDE it: read-only data and instructions share one R-X mapping. The instruction rewrites must
// never see that data -- a constant that happens to encode `svc #0`, `msr tpidr_el0, xN` or an
// x18 operand would be silently corrupted into a trap or a branch. So collect the address ranges
// of the SHF_EXECINSTR sections (.text, .plt, ...) and rewrite only those.
struct code_range { unsigned long lo, hi; uint64_t off; };

static int collect_code_ranges(const Elf64_Ehdr* eh, const unsigned char* f, size_t fsz,
                               struct code_range* out, int max) {
  if (!eh->e_shoff || !eh->e_shnum || eh->e_shentsize < sizeof(Elf64_Shdr)) return -1;
  if (eh->e_shoff > fsz || (uint64_t)eh->e_shnum * eh->e_shentsize > fsz - eh->e_shoff) return -1;
  int n = 0;
  for (unsigned i = 0; i < eh->e_shnum; i++) {
    const Elf64_Shdr* s = (const Elf64_Shdr*)(f + eh->e_shoff + (size_t)i * eh->e_shentsize);
    if (!(s->sh_flags & SHF_ALLOC) || !(s->sh_flags & SHF_EXECINSTR)) continue;
    if (!s->sh_size) continue;
    if (n == max) return -1;                       // too many: fall back to the whole segment
    if (s->sh_type == 8) continue;                 // SHT_NOBITS: no bytes in the file
    if (s->sh_offset > fsz || s->sh_size > fsz - s->sh_offset) return -1;
    out[n].lo  = (unsigned long)s->sh_addr;
    out[n].hi  = (unsigned long)(s->sh_addr + s->sh_size);
    out[n].off = s->sh_offset;
    n++;
  }
  return n;                                        // 0 is legitimate: an exec segment with no code
}

// The x18 rewrite is all-or-nothing for a WHOLE IMAGE, not per section: if .text were veneered
// (x18 living in gmem_guest_x18) while .plt were not (x18 living in the register), the two halves
// would disagree the first time a value crossed between them. So every executable section is
// vetted up front, straight out of the mapped file, before any of them is rewritten.
static int x18_image_is_rewritable(const unsigned char* f, const struct code_range* code,
                                   int ncode, const char* host_path) {
  for (int c = 0; c < ncode; c++) {
    size_t n = (size_t)(code[c].hi - code[c].lo) / 4;
    for (size_t i = 0; i < n; i++) {
      unsigned int w; unsigned fields;
      memcpy(&w, f + code[c].off + i * 4, 4);
      if (gmem_x18_classify(w, &fields) == GMEM_X18_UNSUP) {
        fprintf(stderr, "[elfload] %s: %#lx uses x18 in an encoding that cannot be rewritten "
                        "(%08x); this image keeps the old behaviour, where macOS will zero its "
                        "x18 asynchronously\n", host_path, code[c].lo + i * 4, w);
        return 0;
      }
    }
  }
  return 1;
}

// Copy `len` bytes to guest address `dst`. JIT-backed destinations must go through gmem_map_code()
// (W^X: they are not writable with a plain store on this thread); plain RW pages take a memcpy.
static int place(unsigned long dst, const unsigned char* src, size_t len, int jit) {
  if (len == 0) return 0;
  if (jit) return gmem_map_code((void*)dst, src, len);
  memcpy((void*)dst, src, len);
  return 0;
}

// Build the fastcall thunk at most once. Opt-in via RDFASTCALL=1: the Mach-trap path is always
// correct, so it stays the default until the fast path has proven itself on real workloads.
// Single gate, in fastcall.c — see fastcall_thunk_once(). This used to hold a SECOND copy of
// the RDFASTCALL check and its own build-once, which is how the two patch sites ended up with
// different behaviour.
static unsigned long fc_thunk_once(void) { return fastcall_thunk_once(); }

int elf_load(const char* host_path, struct loaded* out) {
  if (!out) { fprintf(stderr, "[elfload] elf_load: NULL out\n"); return -1; }
  memset(out, 0, sizeof *out);
  if (!host_path || !*host_path) { fprintf(stderr, "[elfload] elf_load: empty path\n"); return -1; }

  // ---- map the file (MAP_PRIVATE: any write is copy-on-write and never reaches the file) ------
  int fd = open(host_path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) { fprintf(stderr, "[elfload] %s: cannot open: %s\n", host_path, strerror(errno)); return -1; }
  struct stat st;
  if (fstat(fd, &st) != 0) {
    fprintf(stderr, "[elfload] %s: fstat: %s\n", host_path, strerror(errno)); close(fd); return -1;
  }
  if (!S_ISREG(st.st_mode)) {
    fprintf(stderr, "[elfload] %s: not a regular file\n", host_path); close(fd); return -1;
  }
  size_t fsz = (size_t)st.st_size;
  if (fsz < sizeof(Elf64_Ehdr)) {
    fprintf(stderr, "[elfload] %s: too small (%zu bytes) to be an ELF\n", host_path, fsz);
    close(fd); return -1;
  }
  unsigned char* f = mmap(NULL, fsz, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  if (f == MAP_FAILED) f = mmap(NULL, fsz, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (f == MAP_FAILED) {
    fprintf(stderr, "[elfload] %s: mmap: %s\n", host_path, strerror(errno)); return -1;
  }

  int rc = -1;                 // everything below jumps to `done` with rc set
  void* region = NULL;
  size_t span = 0;

  // ---- validate the ELF header ----------------------------------------------------------------
  const Elf64_Ehdr* eh = (const Elf64_Ehdr*)f;
  if (memcmp(eh->e_ident, "\177ELF", 4) != 0) {
    fprintf(stderr, "[elfload] %s: not an ELF (bad magic %02x %02x %02x %02x)\n", host_path,
            eh->e_ident[0], eh->e_ident[1], eh->e_ident[2], eh->e_ident[3]);
    goto done;
  }
  if (eh->e_ident[4] != ELFCLASS64) {
    fprintf(stderr, "[elfload] %s: not ELF64 (class %u); need a 64-bit aarch64 binary\n",
            host_path, eh->e_ident[4]);
    goto done;
  }
  if (eh->e_ident[5] != ELFDATA2LSB) {
    fprintf(stderr, "[elfload] %s: not little-endian (data %u)\n", host_path, eh->e_ident[5]);
    goto done;
  }
  if (eh->e_machine != EM_AARCH64) {
    fprintf(stderr, "[elfload] %s: wrong machine %u, need EM_AARCH64 (%d)\n",
            host_path, eh->e_machine, EM_AARCH64);
    goto done;
  }
  if (eh->e_type != ET_DYN && eh->e_type != ET_EXEC) {
    fprintf(stderr, "[elfload] %s: e_type %u is neither ET_EXEC nor ET_DYN\n", host_path, eh->e_type);
    goto done;
  }
  if (eh->e_phnum == 0 || eh->e_phentsize < sizeof(Elf64_Phdr)) {
    fprintf(stderr, "[elfload] %s: bad program header table (phnum=%u phentsize=%u)\n",
            host_path, eh->e_phnum, eh->e_phentsize);
    goto done;
  }
  if (eh->e_phoff > fsz || (uint64_t)eh->e_phnum * eh->e_phentsize > fsz - eh->e_phoff) {
    fprintf(stderr, "[elfload] %s: program headers out of file bounds\n", host_path);
    goto done;
  }

  // ---- collect PT_LOADs (sorted by vaddr), PT_INTERP -------------------------------------------
  struct seg loads[MAX_LOADS];
  int nload = 0;
  const Elf64_Phdr* interp_ph = NULL;
  const Elf64_Phdr* phdr_ph   = NULL;
  unsigned long minv = ~0UL, maxend = 0;

  for (unsigned i = 0; i < eh->e_phnum; i++) {
    const Elf64_Phdr* p = (const Elf64_Phdr*)(f + eh->e_phoff + (size_t)i * eh->e_phentsize);
    if (p->p_type == PT_INTERP) { interp_ph = p; continue; }
    if (p->p_type == PT_PHDR)   { phdr_ph   = p; continue; }
    if (p->p_type != PT_LOAD) continue;
    if (p->p_memsz == 0) continue;
    if (p->p_filesz > p->p_memsz) {
      fprintf(stderr, "[elfload] %s: PT_LOAD %u has filesz %llu > memsz %llu\n", host_path, i,
              (unsigned long long)p->p_filesz, (unsigned long long)p->p_memsz);
      goto done;
    }
    if (p->p_offset > fsz || p->p_filesz > fsz - p->p_offset) {
      fprintf(stderr, "[elfload] %s: PT_LOAD %u extends past end of file\n", host_path, i);
      goto done;
    }
    if (nload == MAX_LOADS) {
      fprintf(stderr, "[elfload] %s: more than %d PT_LOAD segments\n", host_path, MAX_LOADS);
      goto done;
    }
    // insertion sort by p_vaddr so the overlap bookkeeping below is monotonic
    int k = nload++;
    while (k > 0 && loads[k - 1].ph->p_vaddr > p->p_vaddr) { loads[k] = loads[k - 1]; k--; }
    loads[k].ph = p;

    if (p->p_vaddr < minv) minv = p->p_vaddr;
    if (p->p_vaddr + p->p_memsz > maxend) maxend = (unsigned long)(p->p_vaddr + p->p_memsz);
  }
  if (nload == 0) {
    fprintf(stderr, "[elfload] %s: no PT_LOAD segments\n", host_path);
    goto done;
  }

  unsigned long minpg = PDOWN(minv), maxpg = PUP(maxend);
  span = maxpg - minpg;

  // ---- reserve the whole span once, so the image is contiguous --------------------------------
  // NEVER ask for a fixed low address: [0, 4GB) is __PAGEZERO on arm64 macOS and is unmappable.
  // MAP_JIT is also kernel-placed by construction (it cannot be combined with MAP_FIXED), so the
  // load bias is simply whatever the kernel handed back.
  unsigned long bias;
  // ET_EXEC is not relocatable: it must land exactly at its p_vaddr, which is impossible below 4GB.
  if (eh->e_type == ET_EXEC && minpg < FOUR_GB) {
    fprintf(stderr,
            "[elfload] %s: ET_EXEC linked at %#lx — the low 4GB is macOS __PAGEZERO and can never "
            "be mapped. Rebuild as PIE (ET_DYN) or link above 4GB.\n", host_path, minv);
    goto done;
  }
  region = gmem_reserve(span);
  if (!region) {
    fprintf(stderr, "[elfload] %s: gmem_reserve(%zu) failed\n", host_path, span);
    goto done;
  }
  if (eh->e_type == ET_DYN) {
    bias = (unsigned long)region - minpg;
  } else {
    if ((unsigned long)region != minpg) {
      fprintf(stderr,
              "[elfload] %s: ET_EXEC needs its fixed base %#lx but executable guest memory must be "
              "MAP_JIT, which the kernel places itself (got %p). Rebuild as PIE (ET_DYN).\n",
              host_path, minpg, region);
      goto done;
    }
    bias = 0;
  }

  // ---- map every PT_LOAD -----------------------------------------------------------------------
  // One bracket for the whole image: batches the W^X toggles and coalesces the icache flushes.
  struct code_range code[MAX_CODE_SECTS];
  int ncode = collect_code_ranges(eh, f, fsz, code, MAX_CODE_SECTS);
  if (ncode < 0)
    fprintf(stderr, "[elfload] %s: no usable section table; rewriting whole executable segments "
                    "(any .rodata sharing them is at risk)\n", host_path);
  int do_x18 = (ncode > 0) && x18_image_is_rewritable(f, code, ncode, host_path);

  gmem_begin_load();
  int failed = 0;
  unsigned long prev_end = 0;   // page-end of the previously placed segment (0 = none yet)
  int prev_jit = 0;             // ...and whether those pages are JIT-backed

  for (int i = 0; i < nload && !failed; i++) {
    const Elf64_Phdr* p = loads[i].ph;
    int is_exec = (p->p_flags & PF_X) != 0;

    unsigned long va     = bias + p->p_vaddr;
    unsigned long pstart = PDOWN(va);
    unsigned long pend   = PUP(va + p->p_memsz);
    unsigned long gap    = va - pstart;               // bytes of the first page before p_vaddr
    unsigned long cend   = va + p->p_filesz;          // end of the file-backed content

    // Two segments can share a page when the link-time max-page-size is below 16K. [wa, sh) then
    // falls in pages that the previous segment already owns; [sh, pend) is ours alone.
    unsigned long wa = pstart, sh = pstart;
    if (prev_end > pstart) {
      wa = va;
      sh = prev_end < pend ? prev_end : pend;
      if (is_exec && !prev_jit) {
        fprintf(stderr, "[elfload] %s: executable segment %d shares page %#lx with a data segment; "
                        "cannot make it executable\n", host_path, i, pstart);
        failed = 1; break;
      }
    }

    // Stage through a plain heap buffer: a JIT page cannot be written by the kernel (pread) and
    // .bss needs zeroing anyway. Only the part we actually write is allocated — a multi-megabyte
    // .bss stays a fresh (already zero) anonymous mapping.
    unsigned long bufend = (cend > sh ? cend : sh);
    if (bufend > pend) bufend = pend;
    size_t buflen = (size_t)(bufend - pstart);
    unsigned char* buf = NULL;
    if (buflen) {
      buf = calloc(1, buflen);
      if (!buf) {
        fprintf(stderr, "[elfload] %s: out of memory staging segment %d (%zu bytes)\n",
                host_path, i, buflen);
        failed = 1; break;
      }
      // Leading gap: Linux maps whole pages from the file, so reproduce the bytes that precede
      // p_offset when the file actually has them (else leave zeros).
      if (gap) {
        size_t lead = p->p_offset >= gap ? (size_t)gap : (size_t)p->p_offset;
        if (lead) memcpy(buf + gap - lead, f + p->p_offset - lead, lead);
      }
      size_t body = (size_t)p->p_filesz;
      if (gap + body > buflen) body = (size_t)(buflen - gap);
      if (body) memcpy(buf + gap, f + p->p_offset, body);
      // the p_memsz - p_filesz tail (.bss) is already zero: calloc here, fresh anon/JIT pages there
    }

    if (is_exec) {
      // Plant the traps BEFORE these bytes ever reach JIT memory:
      //   svc #0 -> brk #0, msr tpidr_el0,xN -> brk #(0x100+N), mrs xN,tpidr_el0 -> brk #(0x200+N)
      // and move x18 out of the register file (macOS zeroes it behind the guest's back).
      // Only genuine code is rewritten: see collect_code_ranges().
      if (buflen) {
        if (ncode < 0) {
          gmem_rewrite_exec_insns_va(buf, buflen, pstart);      // no section table: whole segment
        } else {
          for (int c = 0; c < ncode; c++) {
            unsigned long lo = bias + code[c].lo, hi = bias + code[c].hi;
            if (lo < pstart) lo = pstart;
            if (hi > pstart + buflen) hi = pstart + buflen;
            if (hi > lo) {
              if (do_x18) gmem_rewrite_exec_insns_va(buf + (lo - pstart), (size_t)(hi - lo), lo);
              else        gmem_rewrite_exec_insns(buf + (lo - pstart), (size_t)(hi - lo));
            }
          }
        }
      }
    } else if (sh < pend) {
      // Non-executable segment: plain RW pages remapped over the JIT span (LINUX mmap flags).
      long r = gmem_mmap(sh, (size_t)(pend - sh), L_PROT_READ | L_PROT_WRITE,
                         L_MAP_PRIVATE | L_MAP_FIXED | L_MAP_ANON, -1, 0);
      if (r < 0 || (unsigned long)r != sh) {
        fprintf(stderr, "[elfload] %s: gmem_mmap(%#lx, %#lx) for segment %d failed (%ld)\n",
                host_path, sh, pend - sh, i, r);
        free(buf); failed = 1; break;
      }
    }

    // [wa, sh): pages owned by the previous segment. [sh, min(cend,pend)): our own pages.
    if (buf && (place(wa, buf + (wa - pstart), (size_t)(sh - wa), prev_jit) != 0 ||
                place(sh, buf + (sh - pstart), (size_t)(bufend - sh), is_exec) != 0)) {
      fprintf(stderr, "[elfload] %s: failed to place segment %d at %#lx\n", host_path, i, va);
      free(buf); failed = 1; break;
    }

    // Trap-free syscall fast path (opt-in: RDFASTCALL=1). Rewrite `brk #0` -> `bl <thunk>` at every
    // site where x30 is provably dead, so a Linux syscall becomes an in-thread call (~8ns) instead
    // of a Mach exception round trip (~9.7us). MUST run here: after the bytes are in JIT, and on the
    // FINAL guest address, because `bl` encodes a PC-relative displacement.
    if (is_exec && buflen) {
      unsigned long th = fc_thunk_once();
      if (th) {
        int patched = 0, skipped = 0, sk;
        if (ncode < 0) {
          patched += fastcall_patch_ex((void*)pstart, buflen, th, &sk); skipped += sk;
        } else {
          for (int c = 0; c < ncode; c++) {
            unsigned long lo = bias + code[c].lo, hi = bias + code[c].hi;
            if (lo < pstart) lo = pstart;
            if (hi > pstart + buflen) hi = pstart + buflen;
            if (hi > lo) { patched += fastcall_patch_ex((void*)lo, (size_t)(hi - lo), th, &sk); skipped += sk; }
          }
        }
        if (getenv("RDVERBOSE"))
          fprintf(stderr, "[fastcall] %s seg%d: patched=%d skipped(x30-live)=%d\n",
                  host_path, i, patched, skipped);
      }
    }

    free(buf);
    prev_end = pend;
    prev_jit = is_exec;
  }
  gmem_end_load();
  if (failed) goto done;

  // ---- results ---------------------------------------------------------------------------------
  out->base  = bias;
  out->entry = bias + eh->e_entry;
  out->phnum = eh->e_phnum;
  out->phent = eh->e_phentsize;

  // AT_PHDR must be the ABSOLUTE address of the phdrs as they exist in guest memory: find the
  // PT_LOAD whose file range contains e_phoff and translate through that segment.
  out->phdr = 0;
  uint64_t phsz = (uint64_t)eh->e_phnum * eh->e_phentsize;
  for (int i = 0; i < nload; i++) {
    const Elf64_Phdr* p = loads[i].ph;
    if (eh->e_phoff >= p->p_offset && eh->e_phoff + phsz <= p->p_offset + p->p_filesz) {
      out->phdr = bias + p->p_vaddr + (eh->e_phoff - p->p_offset);
      break;
    }
  }
  if (!out->phdr && phdr_ph) out->phdr = bias + phdr_ph->p_vaddr;    // fall back to PT_PHDR
  if (!out->phdr)
    fprintf(stderr, "[elfload] %s: warning: program headers are not covered by any PT_LOAD; "
                    "AT_PHDR will be 0\n", host_path);

  if (interp_ph && interp_ph->p_filesz) {
    if (interp_ph->p_offset > fsz || interp_ph->p_filesz > fsz - interp_ph->p_offset) {
      fprintf(stderr, "[elfload] %s: PT_INTERP out of file bounds\n", host_path);
      goto done;
    }
    size_t n = (size_t)interp_ph->p_filesz;
    const char* s = (const char*)(f + interp_ph->p_offset);
    if (n && s[n - 1] == '\0') n--;                                  // drop the NUL
    if (n >= sizeof out->interp) n = sizeof out->interp - 1;
    memcpy(out->interp, s, n);
    out->interp[n] = '\0';
  }

  rc = 0;

done:
  if (rc != 0 && region) gmem_munmap((unsigned long)region, span);
  munmap(f, fsz);
  return rc;
}
