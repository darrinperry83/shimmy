// test_gmem.c — standalone verification of the W^X JIT memory manager.
//
// build & run (the JIT entitlement is REQUIRED to execute MAP_JIT memory):
//   clang -arch arm64 -O2 -o test_gmem test_gmem.c gmem.c \
//     && codesign --force --sign - --entitlements ent.plist test_gmem && ./test_gmem
#include "gmem.h"

#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

// LINUX mmap flag values, as the guest passes them.
#define L_SHARED   0x01
#define L_PRIVATE  0x02
#define L_FIXED    0x10
#define L_ANON     0x20

#define PAGE 16384UL

// aarch64 snippets: `mov x0, #imm ; ret`
#define MOV_X0(imm)  (0xD2800000u | ((unsigned)(imm) << 5))
#define RET          0xD65F03C0u

static int g_tests, g_fails;
static void check(int ok, const char* fmt, ...) {
  va_list ap; va_start(ap, fmt);
  g_tests++; if (!ok) g_fails++;
  printf("  %-4s ", ok ? "ok" : "FAIL");
  vprintf(fmt, ap); putchar('\n'); fflush(stdout);
  va_end(ap);
}
static void section(const char* s) { printf("\n== %s ==\n", s); }

// ---- fault probe: does writing one byte at p raise SIGBUS/SIGSEGV? --------------------
static sigjmp_buf g_jb;
static volatile sig_atomic_t g_faulted;
static void fault_handler(int sig) { (void)sig; g_faulted = 1; siglongjmp(g_jb, 1); }

static int write_faults(volatile unsigned char* p) {
  struct sigaction sa, ob, os;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = fault_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGBUS, &sa, &ob);
  sigaction(SIGSEGV, &sa, &os);
  g_faulted = 0;
  if (sigsetjmp(g_jb, 1) == 0) { *p = 0x11; __asm__ volatile("" ::: "memory"); }
  sigaction(SIGBUS, &ob, NULL);
  sigaction(SIGSEGV, &os, NULL);
  return (int)g_faulted;
}

// ---- helper: publish `mov x0,#imm ; ret` at addr and call it --------------------------
static int publish_and_call(void* addr, unsigned imm, int* mapped_ok) {
  unsigned int code[2] = { MOV_X0(imm), RET };
  int r = gmem_map_code(addr, code, sizeof code);
  if (mapped_ok) *mapped_ok = (r == 0);
  if (r != 0) return -1;
  int (*fn)(void) = (int (*)(void))addr;
  return fn();
}

// =======================================================================================
// 1. reserve + map_code produces genuinely EXECUTABLE code
// =======================================================================================
static void t_reserve_exec(void) {
  section("1. gmem_reserve + gmem_map_code -> executable code");
  void* r = gmem_reserve(PAGE);
  check(r != NULL, "gmem_reserve(16384) returned %p", r);
  if (!r) return;
  check(((uintptr_t)r & (PAGE - 1)) == 0, "reservation is page aligned (%p)", r);

  unsigned int code[2] = { MOV_X0(42), RET };          // mov x0,#42 ; ret
  check(code[0] == 0xD2800540u && code[1] == 0xD65F03C0u,
        "test snippet encodes as {0xD2800540, 0xD65F03C0} (got {0x%08X, 0x%08X})", code[0], code[1]);

  int rc = gmem_map_code(r, code, sizeof code);
  check(rc == 0, "gmem_map_code into the reservation returned %d", rc);

  int (*fn)(void) = (int (*)(void))r;
  int v = fn();                                        // <- actually executes JIT memory
  check(v == 42, "calling the JIT'd function returned %d (want 42)", v);

  // JIT pages are readable, so we can confirm the bytes really landed.
  check(memcmp(r, code, sizeof code) == 0, "bytes in JIT memory match the source");

  // map_code past the end of the region must be refused, not written.
  rc = gmem_map_code((char*)r + PAGE - 4, code, sizeof code);
  check(rc == -1, "gmem_map_code straddling the region end is refused (%d)", rc);
  gmem_munmap((unsigned long)(uintptr_t)r, PAGE);
}

// =======================================================================================
// 2. instruction rewriting
// =======================================================================================
static void t_rewrite(void) {
  section("2. gmem_rewrite_exec_insns");
  // in                     expected out
  struct { unsigned int in, out; const char* what; } v[] = {
    { 0xD4000001u, 0xD4200000u, "svc #0            -> brk #0" },
    { 0xD51BD040u, 0xD4202000u, "msr tpidr_el0,x0  -> brk #0x100" },
    { 0xD51BD045u, 0xD42020A0u, "msr tpidr_el0,x5  -> brk #0x105" },
    { 0xD51BD05Fu, 0xD42023E0u, "msr tpidr_el0,x31 -> brk #0x11F" },
    { 0xD53BD040u, 0xD4204000u, "mrs x0,tpidr_el0  -> brk #0x200" },
    { 0xD53BD053u, 0xD4204260u, "mrs x19,tpidr_el0 -> brk #0x213" },
    { 0xD53BD05Fu, 0xD42043E0u, "mrs x31,tpidr_el0 -> brk #0x21F" },
    // must NOT be touched:
    { 0xD503201Fu, 0xD503201Fu, "nop               -> untouched" },
    { 0xD4000021u, 0xD4000021u, "svc #1            -> untouched" },
    { 0xD51BD060u, 0xD51BD060u, "msr tpidrro_el0   -> untouched" },
    { 0xD53B0020u, 0xD4208000u, "mrs x0,ctr_el0    -> brk #0x400" },
    { 0xD53B0033u, 0xD4208260u, "mrs x19,ctr_el0   -> brk #0x413" },
    { 0xD65F03C0u, 0xD65F03C0u, "ret               -> untouched" },
  };
  const int n = (int)(sizeof v / sizeof v[0]);

  unsigned int buf[32];
  for (int i = 0; i < n; i++) buf[i] = v[i].in;
  gmem_rewrite_exec_insns((unsigned char*)buf, (size_t)n * 4);
  for (int i = 0; i < n; i++)
    check(buf[i] == v[i].out, "%s  (0x%08X -> 0x%08X)", v[i].what, v[i].in, buf[i]);

  // the same pass over a deliberately UNALIGNED staging buffer
  unsigned char raw[8 + 32 * 4];
  unsigned char* p = raw + 1;                                  // 1 mod 4
  for (int i = 0; i < n; i++) memcpy(p + i * 4, &v[i].in, 4);
  gmem_rewrite_exec_insns(p, (size_t)n * 4);
  int allok = 1;
  for (int i = 0; i < n; i++) { unsigned int w; memcpy(&w, p + i * 4, 4); if (w != v[i].out) allok = 0; }
  check(allok, "unaligned staging buffer rewrites identically");

  // a trailing partial word must be left alone (no OOB write)
  unsigned char tail[6] = { 0x01, 0x00, 0x00, 0xD4, 0xAA, 0xBB };  // svc #0 then 2 stray bytes
  gmem_rewrite_exec_insns(tail, sizeof tail);
  check(tail[0] == 0x00 && tail[3] == 0xD4 && tail[4] == 0xAA && tail[5] == 0xBB,
        "trailing partial word left untouched");
}

// =======================================================================================
// 3. anon PROT_NONE reservations are JIT-tracked (and plain RW anon is not)
// =======================================================================================
static void t_prot_none_is_jit(void) {
  section("3. anon PROT_NONE -> tracked JIT reservation");
  long r = gmem_mmap(0, 3 * PAGE, PROT_NONE, L_PRIVATE | L_ANON, -1, 0);
  check(r > 0, "gmem_mmap(PROT_NONE, 3 pages) = %#lx", (unsigned long)r);
  if (r <= 0) return;

  // Only tracked JIT memory accepts gmem_map_code AND can then be executed. This is the proof
  // that ld.so's PROT_NONE DSO-span reservation can later hold MAP_FIXED executable text.
  int mapped = 0;
  int v = publish_and_call((void*)r, 42, &mapped);
  check(mapped, "gmem_map_code into the PROT_NONE reservation succeeded (tracked)");
  check(v == 42, "code executed from the PROT_NONE reservation returned %d (want 42)", v);

  // mprotect on tracked JIT pages is a no-op success (they are already R+X).
  long m = gmem_mprotect((unsigned long)r, 3 * PAGE, PROT_READ | PROT_EXEC);
  check(m == 0, "gmem_mprotect over tracked JIT pages = %ld (no-op success)", m);
  v = ((int (*)(void))r)();
  check(v == 42, "still executable after that mprotect (%d)", v);

  // negative control: a plain RW anon mapping must NOT be tracked
  long p = gmem_mmap(0, PAGE, PROT_READ | PROT_WRITE, L_PRIVATE | L_ANON, -1, 0);
  check(p > 0, "gmem_mmap(PROT_READ|PROT_WRITE anon) = %#lx", (unsigned long)p);
  unsigned int code[2] = { MOV_X0(42), RET };
  check(gmem_map_code((void*)p, code, sizeof code) == -1,
        "gmem_map_code into plain RW anon is refused (not JIT-tracked)");
  *(volatile unsigned char*)p = 0x77;
  check(*(volatile unsigned char*)p == 0x77, "plain RW anon mapping is writable");

  // anon + PROT_EXEC is a JIT reservation too
  long e = gmem_mmap(0, PAGE, PROT_READ | PROT_EXEC, L_PRIVATE | L_ANON, -1, 0);
  mapped = 0;
  v = publish_and_call((void*)e, 7, &mapped);
  check(mapped && v == 7, "anon PROT_EXEC is JIT-tracked and executes (%d)", v);

  gmem_munmap((unsigned long)r, 3 * PAGE);
  gmem_munmap((unsigned long)p, PAGE);
  gmem_munmap((unsigned long)e, PAGE);
}

// =======================================================================================
// 4. MAP_FIXED over a JIT span: writable -> plain RW; exec -> staged + rewritten
// =======================================================================================
static const char* TMPF = "gmem_test_data.bin";

static void t_fixed_over_jit(void) {
  section("4. MAP_FIXED into a tracked JIT span");

  // a small file: [mov x0,#99 ; ret ; svc #0 ; msr tpidr_el0,x3 ; mrs x5,tpidr_el0] + a data page
  unsigned int img[6] = { MOV_X0(99), RET, 0xD4000001u, 0xD51BD043u, 0xD53BD045u, 0xDEADBEEFu };
  int fd = open(TMPF, O_RDWR | O_CREAT | O_TRUNC, 0600);
  check(fd >= 0, "created scratch file %s (fd=%d)", TMPF, fd);
  if (fd < 0) return;
  if (write(fd, img, sizeof img) != (ssize_t)sizeof img) { check(0, "write scratch file"); return; }

  // ld.so's pattern: reserve the whole DSO span PROT_NONE, then MAP_FIXED the segments into it.
  long span = gmem_mmap(0, 4 * PAGE, PROT_NONE, L_PRIVATE | L_ANON, -1, 0);
  check(span > 0, "reserved a 4-page span PROT_NONE at %#lx", (unsigned long)span);
  if (span <= 0) { close(fd); return; }

  // --- 4a: MAP_FIXED read+exec from the file -> staged, rewritten, executable ---
  long t = gmem_mmap((unsigned long)span, PAGE, PROT_READ | PROT_EXEC,
                     L_PRIVATE | L_FIXED, fd, 0);
  check(t == span, "MAP_FIXED PROT_READ|PROT_EXEC at %#lx returned %#lx",
        (unsigned long)span, (unsigned long)t);
  int v = ((int (*)(void))span)();
  check(v == 99, "text mapped from the file executes and returns %d (want 99)", v);
  unsigned int* w = (unsigned int*)span;
  check(w[2] == 0xD4200000u, "svc #0 in the file became brk #0      (0x%08X)", w[2]);
  check(w[3] == 0xD4202060u, "msr tpidr_el0,x3 became brk #0x103    (0x%08X)", w[3]);
  check(w[4] == 0xD42040A0u, "mrs x5,tpidr_el0 became brk #0x205    (0x%08X)", w[4]);
  check(w[5] == 0xDEADBEEFu, "non-matching word left alone          (0x%08X)", w[5]);

  // --- 4b: MAP_FIXED writable over the JIT span -> plain RW, writable by the caller ---
  // This thread runs write-protect(1); if the pages were still MAP_JIT, this write would fault.
  unsigned long d = (unsigned long)span + 2 * PAGE;
  long dr = gmem_mmap(d, PAGE, PROT_READ | PROT_WRITE, L_PRIVATE | L_ANON | L_FIXED, -1, 0);
  check(dr == (long)d, "MAP_FIXED anon RW over JIT at %#lx returned %#lx", d, (unsigned long)dr);
  check(!write_faults((volatile unsigned char*)d), "the caller can write it with no fault");
  volatile unsigned char* q = (volatile unsigned char*)d;
  q[0] = 0xAB; q[PAGE - 1] = 0xCD;
  check(q[0] == 0xAB && q[PAGE - 1] == 0xCD, "first and last byte of the RW page read back");
  check(gmem_map_code((void*)d, img, 8) == -1, "those pages are no longer JIT-tracked");
  check(((int (*)(void))span)() == 99, "the neighbouring JIT text is still executable");

  // --- 4c: MAP_FIXED writable from a FILE over the JIT span -> pread'd contents, writable ---
  unsigned long d2 = (unsigned long)span + 3 * PAGE;
  long dr2 = gmem_mmap(d2, sizeof img, PROT_READ | PROT_WRITE, L_PRIVATE | L_FIXED, fd, 0);
  check(dr2 == (long)d2, "MAP_FIXED file RW over JIT at %#lx returned %#lx", d2, (unsigned long)dr2);
  check(memcmp((void*)d2, img, sizeof img) == 0, "file bytes arrived verbatim (NOT rewritten)");
  check(*(unsigned char*)(d2 + sizeof img) == 0, "tail of the page is zero filled");
  check(!write_faults((volatile unsigned char*)(d2 + 64)), "that mapping is writable");

  // --- 4d: file-backed, NOT fixed, read-only -> plain mmap (untracked) ---
  long ro = gmem_mmap(0, sizeof img, PROT_READ, L_PRIVATE, fd, 0);
  check(ro > 0, "file-backed non-FIXED PROT_READ = %#lx (plain mmap)", (unsigned long)ro);
  check(memcmp((void*)ro, img, sizeof img) == 0, "read-only file mapping has verbatim bytes");
  check(gmem_map_code((void*)ro, img, 8) == -1, "read-only file mapping is not JIT-tracked");
  gmem_munmap((unsigned long)ro, sizeof img);

  // --- 4e: file-backed, NOT fixed, executable -> JIT reserve + stage + rewrite ---
  long x = gmem_mmap(0, sizeof img, PROT_READ | PROT_EXEC, L_PRIVATE, fd, 0);
  check(x > 0, "file-backed non-FIXED PROT_READ|PROT_EXEC = %#lx", (unsigned long)x);
  check(((int (*)(void))x)() == 99, "it executes (returns 99)");
  check(((unsigned int*)x)[2] == 0xD4200000u, "and its svc #0 was rewritten to brk #0");
  gmem_munmap((unsigned long)x, sizeof img);

  gmem_munmap((unsigned long)span, 4 * PAGE);
  close(fd);
  unlink(TMPF);
}

// =======================================================================================
// 5. begin_load / end_load nesting
// =======================================================================================
static void* worker_map(void* arg) {
  void* addr = arg;
  int mapped = 0;
  int v = publish_and_call(addr, 33, &mapped);
  return (void*)(intptr_t)((mapped && v == 33) ? 1 : 0);
}

static void t_load_bracket(void) {
  section("5. gmem_begin_load / gmem_end_load");
  void* r = gmem_reserve(4 * PAGE);
  check(r != NULL, "reserved 4 JIT pages at %p", r);
  if (!r) return;

  unsigned int c7[2]  = { MOV_X0(7),  RET };
  unsigned int c9[2]  = { MOV_X0(9),  RET };
  unsigned int c11[2] = { MOV_X0(11), RET };

  gmem_begin_load();
  gmem_begin_load();                                    // nested
  check(gmem_map_code(r, c7, sizeof c7) == 0, "map_code #1 inside the bracket");
  check(gmem_map_code((char*)r + PAGE, c9, sizeof c9) == 0, "map_code #2 inside the bracket");
  gmem_end_load();                                      // inner: must NOT re-protect

  // Still inside the outer bracket: a RAW write (bypassing gmem_map_code) must succeed. If the
  // inner end_load had re-enabled write protection this byte store would fault.
  int f = write_faults((volatile unsigned char*)((char*)r + 3 * PAGE));
  check(!f, "after the inner end_load, JIT memory is still writable (nesting held)");

  // Another thread must be unaffected by this thread's bracket (the toggle is per-thread).
  pthread_t th; void* tr = 0;
  pthread_create(&th, NULL, worker_map, (char*)r + 2 * PAGE);
  pthread_join(th, &tr);
  check((intptr_t)tr == 1, "a second thread's gmem_map_code works during our bracket");

  check(gmem_map_code((char*)r + 64, c11, sizeof c11) == 0, "map_code #3 inside the bracket");
  gmem_end_load();                                      // outer: re-protect + flush icache

  check(((int (*)(void))r)() == 7, "code written inside the bracket executes (7)");
  check(((int (*)(void))((char*)r + PAGE))() == 9, "second range executes (9)");
  check(((int (*)(void))((char*)r + 64))() == 11, "third range executes (11)");
  check(((int (*)(void))((char*)r + 2 * PAGE))() == 33, "the worker thread's code executes (33)");

  f = write_faults((volatile unsigned char*)((char*)r + 3 * PAGE + 1));
  check(f, "after the outer end_load, raw writes to JIT memory fault again");

  // map_code outside any bracket still behaves exactly as before
  unsigned int c21[2] = { MOV_X0(21), RET };
  check(gmem_map_code((char*)r + 128, c21, sizeof c21) == 0, "map_code outside a bracket");
  check(((int (*)(void))((char*)r + 128))() == 21, "and its code executes (21)");

  gmem_end_load();                                      // unbalanced: must be a harmless no-op
  check(write_faults((volatile unsigned char*)r),
        "an unbalanced end_load is a no-op (JIT stays write protected)");

  gmem_munmap((unsigned long)(uintptr_t)r, 4 * PAGE);
}

// =======================================================================================
// 6. munmap drops tracking; mprotect applies for real off JIT
// =======================================================================================
static void t_unmap_and_mprotect(void) {
  section("6. gmem_munmap tracking + gmem_mprotect off JIT");
  void* r = gmem_reserve(2 * PAGE);
  check(r != NULL, "reserved 2 JIT pages at %p", r);
  if (!r) return;
  unsigned long a = (unsigned long)(uintptr_t)r;
  check(gmem_munmap(a, 2 * PAGE) == 0, "gmem_munmap of the whole region");

  // The region must be gone from the table: a MAP_FIXED RW at the same address now takes the
  // plain-host path and gmem_map_code must refuse it.
  long p = gmem_mmap(a, PAGE, PROT_READ | PROT_WRITE, L_PRIVATE | L_ANON | L_FIXED, -1, 0);
  check(p == (long)a, "re-mapped plain RW at %#lx -> %#lx", a, (unsigned long)p);
  unsigned int code[2] = { MOV_X0(1), RET };
  check(gmem_map_code((void*)a, code, sizeof code) == -1, "the tracked region was dropped");

  // partial munmap must trim, not drop: reserve 3 pages, unmap the middle one
  void* s = gmem_reserve(3 * PAGE);
  unsigned long sa = (unsigned long)(uintptr_t)s;
  check(gmem_munmap(sa + PAGE, PAGE) == 0, "unmapped the middle page of a 3-page region");
  int m0 = 0, m2 = 0;
  int v0 = publish_and_call((void*)sa, 5, &m0);
  int v2 = publish_and_call((void*)(sa + 2 * PAGE), 6, &m2);
  check(m0 && v0 == 5, "the head page is still tracked and executes (%d)", v0);
  check(m2 && v2 == 6, "the tail page is still tracked and executes (%d)", v2);
  check(gmem_map_code((void*)(sa + PAGE), code, sizeof code) == -1,
        "the unmapped middle page is no longer tracked");
  gmem_munmap(sa, 3 * PAGE);

  // real mprotect off JIT
  check(!write_faults((volatile unsigned char*)p), "plain RW page is writable before mprotect");
  check(gmem_mprotect(a, PAGE, PROT_READ) == 0, "gmem_mprotect(PROT_READ) on plain pages");
  check(write_faults((volatile unsigned char*)p), "and it really took effect (write now faults)");
  gmem_munmap(a, PAGE);
}

// =======================================================================================
// 7. negative LINUX errno returns
// =======================================================================================
static void t_errno(void) {
  section("7. negative LINUX errno on failure");
  long r;
  r = gmem_mmap(0, 0, PROT_READ, L_PRIVATE | L_ANON, -1, 0);
  check(r == -22, "gmem_mmap(len=0) = %ld (want -22 EINVAL)", r);
  r = gmem_mmap(0x1234, PAGE, PROT_READ | PROT_WRITE, L_PRIVATE | L_ANON | L_FIXED, -1, 0);
  check(r == -22, "MAP_FIXED at an unaligned address = %ld (want -22 EINVAL)", r);
  r = gmem_mmap(0, PAGE, PROT_READ, L_PRIVATE, 9999, 0);
  check(r == -9, "file mmap on a bad fd = %ld (want -9 EBADF)", r);
  r = gmem_munmap(0, 0);
  check(r == 0, "gmem_munmap(len=0) = %ld (want 0)", r);
  r = gmem_mprotect(0, 0, PROT_READ);
  check(r == 0, "gmem_mprotect(len=0) = %ld (want 0)", r);
  check(gmem_reserve(0) == NULL, "gmem_reserve(0) = NULL");
  check(gmem_map_code(NULL, "x", 1) == -1, "gmem_map_code(NULL) = -1");
  check(gmem_map_code((void*)0x1000, "x", 0) == 0, "gmem_map_code(len=0) = 0 (nothing to do)");
}

int main(void) {
  unlink(TMPF);
  printf("gmem test — page=%lu\n", PAGE);
  gmem_init();
  t_reserve_exec();
  t_rewrite();
  t_prot_none_is_jit();
  t_fixed_over_jit();
  t_load_bracket();
  t_unmap_and_mprotect();
  t_errno();
  printf("\n%s: %d/%d checks passed\n", g_fails ? "FAILED" : "PASSED",
         g_tests - g_fails, g_tests);
  return g_fails ? 1 : 0;
}
