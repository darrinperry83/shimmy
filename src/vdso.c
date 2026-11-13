// vdso.c — synthesized Linux aarch64 vDSO so the guest libc gets trap-free time.
//
// Every guest clock_gettime() currently costs a `svc` -> `brk` -> Mach exception round trip
// (~10us). Linux libc looks for AT_SYSINFO_EHDR in auxv and, when present, parses that image as
// an ELF DSO and calls its exported functions directly instead of issuing syscalls.
//
// So we synthesize a real (tiny, but genuinely valid) ET_DYN aarch64 ELF image in executable
// guest memory:
//
//   off 0        Elf64_Ehdr                     ET_DYN / EM_AARCH64
//   +0x40        Elf64_Phdr[2]                  PT_LOAD (whole image, R+X), PT_DYNAMIC
//                Elf64_Dyn[]                    HASH/STRTAB/SYMTAB/SYMENT/STRSZ/SONAME/
//                                               VERSYM/VERDEF/VERDEFNUM/NULL
//                DT_HASH table                  nbucket,nchain,bucket[],chain[]
//                Elf64_Sym[6]                   NULL + the 5 exported __kernel_* symbols
//                Elf64_Versym[6]                all exported syms -> version index 2
//                Elf64_Verdef[2]                #1 "linux-vdso.so.1" (BASE), #2 "LINUX_2.6.39"
//                .dynstr                        symbol names, soname, version names, section names
//                .text                          5 hand-emitted aarch64 thunks
//                Elf64_Shdr[8]                  section table (not needed at runtime; keeps
//                                               readelf/objdump happy for verification)
//
// The whole image is laid out so that p_offset == p_vaddr == in-memory offset for everything,
// which is what musl's __vdsosym() assumes when it computes
//     base = (char*)ehdr + ph->p_offset - ph->p_vaddr
// and then dereferences every DT_* pointer as base + d_ptr.
//
// The exported functions are aarch64 machine code we emit by hand. They contain NO `svc`
// (that is the entire point) and they are written straight into JIT memory via gmem_map_code(),
// so they never pass through gmem_rewrite_exec_insns(). Each thunk simply tail-forwards the
// Linux C ABI (which is AAPCS, same as ours) into a host C helper whose absolute address is
// materialised with movz/movk. The helper reads macOS's commpage-backed clock via
// clock_gettime_nsec_np() — pure userspace, no trap.
//
// ABI care taken in the thunk:
//   * x18 is Darwin's reserved platform register; Linux code is free to use it as a scratch
//     register, so on entry it may hold garbage. We spill the guest's x18, zero it for the
//     duration of the host call, and restore it before returning to the guest.
//   * The stack is kept 16-byte aligned (single `stp ..., [sp,#-32]!` / `ldp ..., [sp],#32`
//     bracket) and a proper x29/x30 frame is established so Darwin can unwind through us.
//   * The thunk is fully position-independent apart from the absolute 64-bit helper address,
//     which is fine because the guest shares our address space.
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "vdso.h"
#include "gmem.h"

// ---------------------------------------------------------------------------------------------
// Minimal ELF64 types (macOS has no <elf.h>).
// ---------------------------------------------------------------------------------------------
typedef struct { unsigned char e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
                 uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags;
                 uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } Elf64_Ehdr;
typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr,
                 p_filesz, p_memsz, p_align; } Elf64_Phdr;
typedef struct { uint32_t sh_name, sh_type; uint64_t sh_flags, sh_addr, sh_offset, sh_size;
                 uint32_t sh_link, sh_info; uint64_t sh_addralign, sh_entsize; } Elf64_Shdr;
typedef struct { int64_t d_tag; uint64_t d_val; } Elf64_Dyn;
typedef struct { uint32_t st_name; unsigned char st_info, st_other; uint16_t st_shndx;
                 uint64_t st_value, st_size; } Elf64_Sym;
typedef struct { uint16_t vd_version, vd_flags, vd_ndx, vd_cnt; uint32_t vd_hash, vd_aux, vd_next; } Elf64_Verdef;
typedef struct { uint32_t vda_name, vda_next; } Elf64_Verdaux;

#define ET_DYN            3
#define EM_AARCH64        183
#define PT_LOAD           1
#define PT_DYNAMIC        2
#define PF_X              1
#define PF_R              4
#define SHT_PROGBITS      1
#define SHT_STRTAB        3
#define SHT_HASH          5
#define SHT_DYNAMIC       6
#define SHT_DYNSYM        11
#define SHT_GNU_VERDEF    0x6ffffffd
#define SHT_GNU_VERSYM    0x6fffffff
#define SHF_ALLOC         0x2
#define SHF_EXECINSTR     0x4
#define DT_NULL           0
#define DT_HASH           4
#define DT_STRTAB         5
#define DT_SYMTAB         6
#define DT_STRSZ          10
#define DT_SYMENT         11
#define DT_SONAME         14
#define DT_VERSYM         0x6ffffff0
#define DT_VERDEF         0x6ffffffc
#define DT_VERDEFNUM      0x6ffffffd
#define STB_GLOBAL        1
#define STT_FUNC          2
#define STV_DEFAULT       0
#define VER_FLG_BASE      1

#define VDSO_IMG_SIZE     16384u   // one guest page
#define VDSO_NSYM         6        // STN_UNDEF + 5 exports
#define VDSO_NBUCKET      7
#define VDSO_NDYN         10
#define VDSO_NPHDR        2
#define VDSO_NSHDR        8
#define VDSO_STRCAP       512
#define VDSO_THUNK_SZ     48

#define VDSO_VERSION_NAME "LINUX_2.6.39"
#define VDSO_SONAME       "linux-vdso.so.1"

// ---------------------------------------------------------------------------------------------
// Linux userspace structs the guest hands us. aarch64 Linux: both members are 64-bit.
// ---------------------------------------------------------------------------------------------
struct lx_timespec { int64_t tv_sec; int64_t tv_nsec; };
struct lx_timeval  { int64_t tv_sec; int64_t tv_usec; };
struct lx_timezone { int32_t tz_minuteswest; int32_t tz_dsttime; };

// Linux errno values (raw Linux ABI, as per the shim's syscall convention).
#define LX_EFAULT   14
#define LX_EINVAL   22
#define LX_ENOSYS   38

// Linux clock ids.
#define LX_CLOCK_REALTIME            0
#define LX_CLOCK_MONOTONIC           1
#define LX_CLOCK_PROCESS_CPUTIME_ID  2
#define LX_CLOCK_THREAD_CPUTIME_ID   3
#define LX_CLOCK_MONOTONIC_RAW       4
#define LX_CLOCK_REALTIME_COARSE     5
#define LX_CLOCK_MONOTONIC_COARSE    6
#define LX_CLOCK_BOOTTIME            7
#define LX_CLOCK_REALTIME_ALARM      8
#define LX_CLOCK_BOOTTIME_ALARM      9
#define LX_CLOCK_TAI                 11

// ---------------------------------------------------------------------------------------------
// Host-side helpers. These run in *our* binary, with the guest's registers still live, so keep
// them small and free of anything that could re-enter the guest.
//
// Return values follow the raw Linux ABI: 0 (or a value) on success, negative errno on failure.
// Returning an error other than -EINVAL makes musl fall through to the real syscall, which is
// exactly the behaviour we want for clocks we cannot serve here.
// ---------------------------------------------------------------------------------------------

// 0 on success (ns written), negative Linux errno otherwise.
static long vdso_ns_for_clock(long clk, uint64_t* out) {
  uint64_t ns;
  int allow_zero = 0;
  switch (clk) {
    case LX_CLOCK_REALTIME:
    case LX_CLOCK_REALTIME_COARSE:
    case LX_CLOCK_REALTIME_ALARM:
    case LX_CLOCK_TAI:
      ns = clock_gettime_nsec_np(CLOCK_REALTIME);                 break;
    // Linux CLOCK_MONOTONIC does NOT advance across suspend -> macOS CLOCK_UPTIME_RAW.
    case LX_CLOCK_MONOTONIC:
    case LX_CLOCK_MONOTONIC_RAW:
    case LX_CLOCK_MONOTONIC_COARSE:
      ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);               break;
    // Linux CLOCK_BOOTTIME DOES advance across suspend -> macOS CLOCK_MONOTONIC_RAW.
    case LX_CLOCK_BOOTTIME:
    case LX_CLOCK_BOOTTIME_ALARM:
      ns = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);            break;
    case LX_CLOCK_PROCESS_CPUTIME_ID:
      ns = clock_gettime_nsec_np(CLOCK_PROCESS_CPUTIME_ID); allow_zero = 1; break;
    case LX_CLOCK_THREAD_CPUTIME_ID:
      ns = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);  allow_zero = 1; break;
    default:
      return -LX_ENOSYS;   // dynamic/unknown clock: let libc issue the real syscall
  }
  if (!ns && !allow_zero) return -LX_ENOSYS;
  *out = ns;
  return 0;
}

__attribute__((used, noinline))
static long vdso_impl_clock_gettime(long clk, struct lx_timespec* ts) {
  uint64_t ns; long r = vdso_ns_for_clock(clk, &ns);
  if (r) return r;
  if (!ts) return -LX_EFAULT;
  ts->tv_sec  = (int64_t)(ns / 1000000000ull);
  ts->tv_nsec = (int64_t)(ns % 1000000000ull);
  return 0;
}

__attribute__((used, noinline))
static long vdso_impl_gettimeofday(struct lx_timeval* tv, struct lx_timezone* tz) {
  if (tv) {
    uint64_t ns = clock_gettime_nsec_np(CLOCK_REALTIME);
    if (!ns) return -LX_ENOSYS;
    tv->tv_sec  = (int64_t)(ns / 1000000000ull);
    tv->tv_usec = (int64_t)((ns % 1000000000ull) / 1000ull);
  }
  if (tz) { tz->tz_minuteswest = 0; tz->tz_dsttime = 0; }
  return 0;
}

__attribute__((used, noinline))
static long vdso_impl_clock_getres(long clk, struct lx_timespec* ts) {
  int64_t res;
  switch (clk) {
    case LX_CLOCK_REALTIME_COARSE:
    case LX_CLOCK_MONOTONIC_COARSE: res = 1000000; break;   // Linux coarse clocks: ~1 tick
    case LX_CLOCK_REALTIME:
    case LX_CLOCK_MONOTONIC:
    case LX_CLOCK_MONOTONIC_RAW:
    case LX_CLOCK_PROCESS_CPUTIME_ID:
    case LX_CLOCK_THREAD_CPUTIME_ID:
    case LX_CLOCK_BOOTTIME:
    case LX_CLOCK_REALTIME_ALARM:
    case LX_CLOCK_BOOTTIME_ALARM:
    case LX_CLOCK_TAI:              res = 1; break;
    default:                        return -LX_ENOSYS;
  }
  if (ts) { ts->tv_sec = 0; ts->tv_nsec = res; }   // NULL ts is legal for clock_getres
  return 0;
}

__attribute__((used, noinline))
static long vdso_impl_time(int64_t* tp) {
  uint64_t ns = clock_gettime_nsec_np(CLOCK_REALTIME);
  if (!ns) return -LX_ENOSYS;
  int64_t s = (int64_t)(ns / 1000000000ull);
  if (tp) *tp = s;
  return s;
}

__attribute__((used, noinline))
static long vdso_impl_getcpu(uint32_t* cpu, uint32_t* node, void* unused) {
  (void)unused;
  if (cpu)  *cpu  = 0;
  if (node) *node = 0;
  return 0;
}

// ---------------------------------------------------------------------------------------------
// aarch64 thunk emitter.
//
//   stp  x29, x30, [sp, #-32]!   ; 16-byte-aligned frame, save fp/lr
//   mov  x29, sp
//   str  x18, [sp, #16]          ; spill the guest's platform register
//   mov  x18, xzr                ; Darwin reserves x18 and wants it zero in user code
//   movz x9,  #a0
//   movk x9,  #a1, lsl #16
//   movk x9,  #a2, lsl #32
//   movk x9,  #a3, lsl #48       ; absolute address of the host C helper
//   blr  x9                      ; x0..x7 pass straight through (Linux ABI == AAPCS)
//   ldr  x18, [sp, #16]          ; restore the guest's x18
//   ldp  x29, x30, [sp], #32
//   ret
//
// No svc. No tpidr_el0 access. Nothing PC-relative.
// ---------------------------------------------------------------------------------------------
static void vdso_emit_thunk(unsigned char* img, size_t off, const void* host_fn) {
  uint64_t a = (uint64_t)(uintptr_t)host_fn;
  uint32_t ins[12] = {
    0xA9BE7BFDu,                                              // stp  x29, x30, [sp, #-32]!
    0x910003FDu,                                              // mov  x29, sp
    0xF9000BF2u,                                              // str  x18, [sp, #16]
    0xAA1F03F2u,                                              // mov  x18, xzr
    0xD2800009u | (uint32_t)(((a >>  0) & 0xffff) << 5),      // movz x9, #imm0
    0xF2A00009u | (uint32_t)(((a >> 16) & 0xffff) << 5),      // movk x9, #imm1, lsl #16
    0xF2C00009u | (uint32_t)(((a >> 32) & 0xffff) << 5),      // movk x9, #imm2, lsl #32
    0xF2E00009u | (uint32_t)(((a >> 48) & 0xffff) << 5),      // movk x9, #imm3, lsl #48
    0xD63F0120u,                                              // blr  x9
    0xF9400BF2u,                                              // ldr  x18, [sp, #16]
    0xA8C27BFDu,                                              // ldp  x29, x30, [sp], #32
    0xD65F03C0u,                                              // ret
  };
  memcpy(img + off, ins, sizeof ins);
}

// ---------------------------------------------------------------------------------------------
// Image construction
// ---------------------------------------------------------------------------------------------
static unsigned long g_vdso_base = 0;

static uint32_t vdso_elf_hash(const char* s) {
  const unsigned char* p = (const unsigned char*)s;
  uint32_t h = 0, g;
  while (*p) { h = (h << 4) + *p++; g = h & 0xf0000000u; if (g) h ^= g >> 24; h &= ~g; }
  return h;
}

struct strtab { char* buf; uint32_t len, cap; };
static uint32_t str_add(struct strtab* st, const char* s) {
  uint32_t n = (uint32_t)strlen(s) + 1;
  if (st->len + n > st->cap) return 0;
  uint32_t at = st->len;
  memcpy(st->buf + at, s, n);
  st->len += n;
  return at;
}

static size_t align_up(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

void vdso_build(void) {
  if (g_vdso_base) return;

  static unsigned char img[VDSO_IMG_SIZE];   // plain RW staging buffer (BSS)
  memset(img, 0, sizeof img);

  const char* symnames[VDSO_NSYM] = {
    "",
    "__kernel_clock_gettime",
    "__kernel_gettimeofday",
    "__kernel_clock_getres",
    "__kernel_time",
    "__kernel_getcpu",
  };
  const void* symimpl[VDSO_NSYM] = {
    0,
    (const void*)&vdso_impl_clock_gettime,
    (const void*)&vdso_impl_gettimeofday,
    (const void*)&vdso_impl_clock_getres,
    (const void*)&vdso_impl_time,
    (const void*)&vdso_impl_getcpu,
  };

  // ---- layout (offset == vaddr for every byte in the image) ----
  size_t off = sizeof(Elf64_Ehdr);                                   // 0x40
  size_t o_phdr   = off;                     off += VDSO_NPHDR * sizeof(Elf64_Phdr);
  off = align_up(off, 8);
  size_t o_dyn    = off;                     off += VDSO_NDYN * sizeof(Elf64_Dyn);
  size_t o_hash   = off;                     off += (2 + VDSO_NBUCKET + VDSO_NSYM) * 4;
  off = align_up(off, 8);
  size_t o_sym    = off;                     off += VDSO_NSYM * sizeof(Elf64_Sym);
  size_t o_versym = off;                     off += VDSO_NSYM * 2;
  off = align_up(off, 8);
  size_t o_verdef = off;                     off += 2 * sizeof(Elf64_Verdef) + 2 * sizeof(Elf64_Verdaux);
  size_t o_str    = off;                     off += VDSO_STRCAP;
  off = align_up(off, 16);
  size_t o_text   = off;                     off += (VDSO_NSYM - 1) * VDSO_THUNK_SZ;
  off = align_up(off, 8);
  size_t o_shdr   = off;                     off += VDSO_NSHDR * sizeof(Elf64_Shdr);
  if (off > VDSO_IMG_SIZE) return;           // cannot happen; bail safely (auxv base stays 0)

  // ---- .dynstr ----
  struct strtab st = { (char*)(img + o_str), 0, VDSO_STRCAP };
  st.len = 1;                                                        // index 0 == ""
  uint32_t sym_stroff[VDSO_NSYM]; sym_stroff[0] = 0;
  for (int i = 1; i < VDSO_NSYM; i++) sym_stroff[i] = str_add(&st, symnames[i]);
  uint32_t so_off  = str_add(&st, VDSO_SONAME);
  uint32_t ver_off = str_add(&st, VDSO_VERSION_NAME);
  // section names live in the same string table (e_shstrndx points at .dynstr)
  uint32_t sn_hash = str_add(&st, ".hash");
  uint32_t sn_dsym = str_add(&st, ".dynsym");
  uint32_t sn_dstr = str_add(&st, ".dynstr");
  uint32_t sn_vsym = str_add(&st, ".gnu.version");
  uint32_t sn_vdef = str_add(&st, ".gnu.version_d");
  uint32_t sn_text = str_add(&st, ".text");
  uint32_t sn_dyn  = str_add(&st, ".dynamic");
  if (!ver_off || !so_off || !sn_dyn) return;                        // strtab overflow: bail safely

  // ---- .text : one thunk per exported symbol ----
  for (int i = 1; i < VDSO_NSYM; i++)
    vdso_emit_thunk(img, o_text + (size_t)(i - 1) * VDSO_THUNK_SZ, symimpl[i]);

  // ---- .dynsym ----
  Elf64_Sym* sym = (Elf64_Sym*)(img + o_sym);
  for (int i = 1; i < VDSO_NSYM; i++) {
    sym[i].st_name  = sym_stroff[i];
    sym[i].st_info  = (unsigned char)((STB_GLOBAL << 4) | STT_FUNC);
    sym[i].st_other = STV_DEFAULT;
    sym[i].st_shndx = 6;                                             // .text (must be != SHN_UNDEF)
    sym[i].st_value = o_text + (uint64_t)(i - 1) * VDSO_THUNK_SZ;
    sym[i].st_size  = VDSO_THUNK_SZ;
  }

  // ---- DT_HASH : nbucket, nchain, bucket[nbucket], chain[nchain] ----
  uint32_t* hash = (uint32_t*)(img + o_hash);
  hash[0] = VDSO_NBUCKET;
  hash[1] = VDSO_NSYM;
  uint32_t* bucket = hash + 2;
  uint32_t* chain  = bucket + VDSO_NBUCKET;
  for (int i = 1; i < VDSO_NSYM; i++) {
    uint32_t b = vdso_elf_hash(symnames[i]) % VDSO_NBUCKET;
    chain[i]  = bucket[b];                                           // prepend; 0 terminates
    bucket[b] = (uint32_t)i;
  }

  // ---- .gnu.version / .gnu.version_d ----
  uint16_t* versym = (uint16_t*)(img + o_versym);
  versym[0] = 0;
  for (int i = 1; i < VDSO_NSYM; i++) versym[i] = 2;                 // -> verdef ndx 2

  Elf64_Verdef*  vd1 = (Elf64_Verdef*)(img + o_verdef);
  Elf64_Verdaux* va1 = (Elf64_Verdaux*)(img + o_verdef + sizeof(Elf64_Verdef));
  Elf64_Verdef*  vd2 = (Elf64_Verdef*)(img + o_verdef + 28);
  Elf64_Verdaux* va2 = (Elf64_Verdaux*)(img + o_verdef + 28 + sizeof(Elf64_Verdef));
  vd1->vd_version = 1; vd1->vd_flags = VER_FLG_BASE; vd1->vd_ndx = 1; vd1->vd_cnt = 1;
  vd1->vd_hash = vdso_elf_hash(VDSO_SONAME);  vd1->vd_aux = 20; vd1->vd_next = 28;
  va1->vda_name = so_off;  va1->vda_next = 0;
  vd2->vd_version = 1; vd2->vd_flags = 0;            vd2->vd_ndx = 2; vd2->vd_cnt = 1;
  vd2->vd_hash = vdso_elf_hash(VDSO_VERSION_NAME); vd2->vd_aux = 20; vd2->vd_next = 0;
  va2->vda_name = ver_off; va2->vda_next = 0;

  // ---- PT_DYNAMIC contents ----
  Elf64_Dyn* dyn = (Elf64_Dyn*)(img + o_dyn);
  int d = 0;
  dyn[d].d_tag = DT_HASH;       dyn[d++].d_val = o_hash;
  dyn[d].d_tag = DT_STRTAB;     dyn[d++].d_val = o_str;
  dyn[d].d_tag = DT_SYMTAB;     dyn[d++].d_val = o_sym;
  dyn[d].d_tag = DT_SYMENT;     dyn[d++].d_val = sizeof(Elf64_Sym);
  dyn[d].d_tag = DT_STRSZ;      dyn[d++].d_val = st.len;
  dyn[d].d_tag = DT_SONAME;     dyn[d++].d_val = so_off;
  dyn[d].d_tag = DT_VERSYM;     dyn[d++].d_val = o_versym;
  dyn[d].d_tag = DT_VERDEF;     dyn[d++].d_val = o_verdef;
  dyn[d].d_tag = DT_VERDEFNUM;  dyn[d++].d_val = 2;
  dyn[d].d_tag = DT_NULL;       dyn[d++].d_val = 0;
  // d == VDSO_NDYN

  // ---- program headers ----
  Elf64_Phdr* ph = (Elf64_Phdr*)(img + o_phdr);
  ph[0].p_type = PT_LOAD;    ph[0].p_flags = PF_R | PF_X;
  ph[0].p_offset = 0; ph[0].p_vaddr = 0; ph[0].p_paddr = 0;
  ph[0].p_filesz = VDSO_IMG_SIZE; ph[0].p_memsz = VDSO_IMG_SIZE; ph[0].p_align = 16384;
  ph[1].p_type = PT_DYNAMIC; ph[1].p_flags = PF_R;
  ph[1].p_offset = o_dyn; ph[1].p_vaddr = o_dyn; ph[1].p_paddr = 0;
  ph[1].p_filesz = VDSO_NDYN * sizeof(Elf64_Dyn);
  ph[1].p_memsz  = VDSO_NDYN * sizeof(Elf64_Dyn);
  ph[1].p_align  = 8;

  // ---- section headers (runtime loaders ignore these; tools like them) ----
  Elf64_Shdr* sh = (Elf64_Shdr*)(img + o_shdr);
  #define SET_SH(i, nm, ty, fl, o, sz, lnk, inf, al, es) \
    do { sh[i].sh_name = (nm); sh[i].sh_type = (ty); sh[i].sh_flags = (fl); \
         sh[i].sh_addr = (o); sh[i].sh_offset = (o); sh[i].sh_size = (sz); \
         sh[i].sh_link = (lnk); sh[i].sh_info = (inf); sh[i].sh_addralign = (al); \
         sh[i].sh_entsize = (es); } while (0)
  SET_SH(1, sn_hash, SHT_HASH,        SHF_ALLOC, o_hash,   (2+VDSO_NBUCKET+VDSO_NSYM)*4u, 2, 0, 4, 4);
  SET_SH(2, sn_dsym, SHT_DYNSYM,      SHF_ALLOC, o_sym,    VDSO_NSYM*sizeof(Elf64_Sym),   3, 1, 8, sizeof(Elf64_Sym));
  SET_SH(3, sn_dstr, SHT_STRTAB,      SHF_ALLOC, o_str,    st.len,                        0, 0, 1, 0);
  SET_SH(4, sn_vsym, SHT_GNU_VERSYM,  SHF_ALLOC, o_versym, VDSO_NSYM*2u,                  2, 0, 2, 2);
  SET_SH(5, sn_vdef, SHT_GNU_VERDEF,  SHF_ALLOC, o_verdef, 56,                            3, 2, 8, 0);
  SET_SH(6, sn_text, SHT_PROGBITS,    SHF_ALLOC|SHF_EXECINSTR, o_text,
                                                 (VDSO_NSYM-1)*(uint64_t)VDSO_THUNK_SZ,   0, 0, 16, 0);
  SET_SH(7, sn_dyn,  SHT_DYNAMIC,     SHF_ALLOC, o_dyn,    VDSO_NDYN*sizeof(Elf64_Dyn),   3, 0, 8, sizeof(Elf64_Dyn));
  #undef SET_SH

  // ---- ELF header ----
  Elf64_Ehdr* eh = (Elf64_Ehdr*)img;
  eh->e_ident[0] = 0x7f; eh->e_ident[1] = 'E'; eh->e_ident[2] = 'L'; eh->e_ident[3] = 'F';
  eh->e_ident[4] = 2;    // ELFCLASS64
  eh->e_ident[5] = 1;    // ELFDATA2LSB
  eh->e_ident[6] = 1;    // EV_CURRENT
  eh->e_ident[7] = 0;    // ELFOSABI_SYSV
  eh->e_type      = ET_DYN;
  eh->e_machine   = EM_AARCH64;
  eh->e_version   = 1;
  eh->e_entry     = 0;
  eh->e_phoff     = o_phdr;
  eh->e_shoff     = o_shdr;
  eh->e_flags     = 0;
  eh->e_ehsize    = sizeof(Elf64_Ehdr);
  eh->e_phentsize = sizeof(Elf64_Phdr);
  eh->e_phnum     = VDSO_NPHDR;
  eh->e_shentsize = sizeof(Elf64_Shdr);
  eh->e_shnum     = VDSO_NSHDR;
  eh->e_shstrndx  = 3;                       // .dynstr doubles as the section-name strtab

  // ---- publish into executable guest memory ----
  void* base = gmem_reserve(VDSO_IMG_SIZE);
  if (!base) return;                         // stays 0 -> libc falls back to real syscalls
  if (gmem_map_code(base, img, VDSO_IMG_SIZE) != 0) return;
  g_vdso_base = (unsigned long)(uintptr_t)base;
}

unsigned long vdso_auxv_base(void) { return g_vdso_base; }
