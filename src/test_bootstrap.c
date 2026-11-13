// test_bootstrap.c — standalone unit test for bootstrap.c.
//
// Builds initial stacks and then WALKS them back exactly the way Linux libc does:
//   long argc = *sp; char** argv = sp+1; char** envp = argv+argc+1;
//   Elf64_auxv_t* auxv = (after the envp NULL);
// asserting every field round-trips. Also forks a child that actually enter_guest()s into a
// hand-written probe, to prove SP/x29/x30 are what the guest ABI requires.
//
// build:  clang -arch arm64 -O2 -Wall -Wextra -o test_bootstrap test_bootstrap.c bootstrap.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>
#include "bootstrap.h"

// vdso.c is a separate module; stub it so this test links standalone. Returning 0 is a legal
// AT_SYSINFO_EHDR value ("no vDSO"), and the test asserts that 0 is what lands in the auxv.
unsigned long vdso_auxv_base(void) { return 0; }

#define AT_NULL          0
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
#define AUX_MAX         64

static int failures, checks;
#define CHECK(cond, ...)                                                      \
  do {                                                                        \
    checks++;                                                                 \
    if (!(cond)) {                                                            \
      failures++;                                                             \
      fprintf(stderr, "  FAIL %s:%d: ", __func__, __LINE__);                  \
      fprintf(stderr, __VA_ARGS__);                                           \
      fprintf(stderr, "\n");                                                  \
    }                                                                         \
  } while (0)

// ---------------------------------------------------------------------------
// The libc-style walker.
// ---------------------------------------------------------------------------
struct walk {
  long           argc;
  char**         argv;
  char**         envp;
  int            envc;
  int            npairs;                 // auxv pairs including the AT_NULL terminator
  int            seen[AUX_MAX];
  unsigned long  val[AUX_MAX];
  int            terminated;             // saw AT_NULL
};

static void walk_stack(void* sp, struct walk* w) {
  memset(w, 0, sizeof *w);
  unsigned long* v = (unsigned long*)sp;
  size_t i = 0;

  w->argc = (long)v[i++];                       // 1. argc
  w->argv = (char**)&v[i];                      // 2. argv[]
  i += (size_t)w->argc;
  if (v[i] != 0) { fprintf(stderr, "  FATAL: argv not NULL-terminated\n"); failures++; return; }
  i++;                                          // 3. argv NULL

  w->envp = (char**)&v[i];                      // 4. envp[]
  while (v[i] != 0) i++;
  w->envc = (int)((unsigned long*)&v[i] - (unsigned long*)w->envp);
  i++;                                          // 5. envp NULL

  for (;;) {                                    // 6. auxv pairs
    unsigned long type = v[i], val = v[i + 1];
    i += 2;
    w->npairs++;
    if (type < AUX_MAX) { w->seen[type]++; w->val[type] = val; }
    if (type == AT_NULL) { w->terminated = (val == 0); break; }
    if (w->npairs > 128) { fprintf(stderr, "  FATAL: auxv runaway\n"); failures++; return; }
  }
}

// ---------------------------------------------------------------------------
// One full build + walk + verify pass.
// ---------------------------------------------------------------------------
static void run_case(const char* name, int argc, char** argv, char** envp,
                     const struct prog_info* p, size_t stksz) {
  printf("-- case: %s (argc=%d)\n", name, argc);
  unsigned char* stack = (unsigned char*)malloc(stksz);
  if (!stack) { fprintf(stderr, "malloc failed\n"); exit(1); }
  memset(stack, 0xA5, stksz);                   // poison: nothing may be read that we didn't write

  void* sp = build_stack(stack, stksz, argc, argv, envp, p);
  CHECK(sp != NULL, "build_stack returned NULL");
  if (!sp) { free(stack); return; }

  uintptr_t lo = (uintptr_t)stack, hi = lo + stksz;
  CHECK(((uintptr_t)sp & 15) == 0, "sp %p is not 16-byte aligned", sp);
  CHECK((uintptr_t)sp >= lo && (uintptr_t)sp < hi, "sp %p outside stack region", sp);

  struct walk w;
  walk_stack(sp, &w);

  // --- argc / argv ---
  CHECK(w.argc == argc, "argc: got %ld want %d", w.argc, argc);
  for (int i = 0; i < argc; i++) {
    uintptr_t a = (uintptr_t)w.argv[i];
    CHECK(a > (uintptr_t)sp && a < hi, "argv[%d] ptr %#lx not in the string block", i, a);
    CHECK(a && strcmp(w.argv[i], argv[i]) == 0,
          "argv[%d]: got '%s' want '%s'", i, a ? w.argv[i] : "(null)", argv[i]);
  }
  // Linux keeps argv strings contiguous and ascending (setproctitle/ps rely on it).
  for (int i = 1; i < argc; i++)
    CHECK((uintptr_t)w.argv[i] > (uintptr_t)w.argv[i - 1],
          "argv strings not ascending at %d", i);

  // --- envp ---
  int envc = 0; while (envp && envp[envc]) envc++;
  CHECK(w.envc == envc, "envc: got %d want %d", w.envc, envc);
  for (int i = 0; i < envc && i < w.envc; i++) {
    uintptr_t a = (uintptr_t)w.envp[i];
    CHECK(a > (uintptr_t)sp && a < hi, "envp[%d] ptr %#lx not in the string block", i, a);
    CHECK(a && strcmp(w.envp[i], envp[i]) == 0,
          "envp[%d]: got '%s' want '%s'", i, a ? w.envp[i] : "(null)", envp[i]);
  }
  // env block sits above the argv block.
  if (argc > 0 && envc > 0)
    CHECK((uintptr_t)w.envp[0] > (uintptr_t)w.argv[argc - 1], "env block is not above argv block");

  // --- auxv ---
  CHECK(w.terminated, "auxv is not AT_NULL/0 terminated");
  CHECK(w.val[AT_PHDR]   == p->phdr,             "AT_PHDR %#lx want %#lx", w.val[AT_PHDR], p->phdr);
  CHECK(w.val[AT_PHENT]  == (unsigned long)p->phent, "AT_PHENT %lu want %d", w.val[AT_PHENT], p->phent);
  CHECK(w.val[AT_PHNUM]  == (unsigned long)p->phnum, "AT_PHNUM %lu want %d", w.val[AT_PHNUM], p->phnum);
  CHECK(w.val[AT_PAGESZ] == 16384,               "AT_PAGESZ %lu want 16384", w.val[AT_PAGESZ]);
  CHECK(w.val[AT_BASE]   == p->interp_base,      "AT_BASE %#lx want %#lx", w.val[AT_BASE], p->interp_base);
  CHECK(w.val[AT_ENTRY]  == p->entry,            "AT_ENTRY %#lx want %#lx (must be the MAIN entry)",
        w.val[AT_ENTRY], p->entry);
  CHECK(w.val[AT_FLAGS]  == 0,                   "AT_FLAGS %lu want 0", w.val[AT_FLAGS]);
  CHECK(w.val[AT_UID] == 501 && w.val[AT_EUID] == 501, "AT_UID/EUID %lu/%lu want 501",
        w.val[AT_UID], w.val[AT_EUID]);
  CHECK(w.val[AT_GID] == 20 && w.val[AT_EGID] == 20,   "AT_GID/EGID %lu/%lu want 20",
        w.val[AT_GID], w.val[AT_EGID]);
  CHECK(w.val[AT_HWCAP] == 0 && w.val[AT_HWCAP2] == 0, "AT_HWCAP/2 must be 0");
  CHECK(w.val[AT_CLKTCK] == 100,                 "AT_CLKTCK %lu want 100", w.val[AT_CLKTCK]);
  CHECK(w.val[AT_SECURE] == 0,                   "AT_SECURE %lu want 0", w.val[AT_SECURE]);
  CHECK(w.val[AT_SYSINFO_EHDR] == vdso_auxv_base(), "AT_SYSINFO_EHDR %#lx want %#lx",
        w.val[AT_SYSINFO_EHDR], vdso_auxv_base());

  const int required[] = { AT_PHDR, AT_PHENT, AT_PHNUM, AT_PAGESZ, AT_BASE, AT_FLAGS, AT_ENTRY,
                           AT_UID, AT_EUID, AT_GID, AT_EGID, AT_HWCAP, AT_CLKTCK, AT_SECURE,
                           AT_RANDOM, AT_HWCAP2, AT_EXECFN, AT_SYSINFO_EHDR, AT_NULL };
  for (size_t i = 0; i < sizeof required / sizeof required[0]; i++) {
    CHECK(w.seen[required[i]] == 1, "auxv type %d appears %d times (want exactly 1)",
          required[i], w.seen[required[i]]);
  }
  CHECK(w.npairs == (int)(sizeof required / sizeof required[0]),
        "auxv pair count %d want %zu", w.npairs, sizeof required / sizeof required[0]);

  // AT_RANDOM: 16 readable bytes inside the region, and not our 0xA5 poison.
  uintptr_t r = w.val[AT_RANDOM];
  CHECK(r > (uintptr_t)sp && r + 16 <= hi, "AT_RANDOM %#lx does not point at 16 in-region bytes", r);
  if (r > (uintptr_t)sp && r + 16 <= hi) {
    unsigned char copy[16];
    memcpy(copy, (void*)r, 16);                 // must be readable
    int poison = 1;
    for (int i = 0; i < 16; i++) if (copy[i] != 0xA5) poison = 0;
    CHECK(!poison, "AT_RANDOM bytes were never written (still poison)");
    CHECK(r + 16 == (uintptr_t)stack + stksz - (((uintptr_t)stack + stksz) & 15),
          "AT_RANDOM is not the topmost object on the stack");
  }

  // AT_EXECFN must be the argv[0] string itself.
  uintptr_t fn = w.val[AT_EXECFN];
  CHECK(fn > (uintptr_t)sp && fn < hi, "AT_EXECFN %#lx not in the string block", fn);
  if (argc > 0) {
    CHECK(fn == (uintptr_t)w.argv[0], "AT_EXECFN %#lx != argv[0] %p", fn, (void*)w.argv[0]);
    CHECK(strcmp((char*)fn, argv[0]) == 0, "AT_EXECFN string '%s' want '%s'", (char*)fn, argv[0]);
  } else {
    CHECK(*(char*)fn == '\0', "AT_EXECFN with no argv should be an empty string");
  }

  // Nothing in the vector may overlap the string block.
  uintptr_t vec_end = (uintptr_t)sp + (size_t)(1 + w.argc + 1 + w.envc + 1 + 2 * w.npairs) * 8;
  uintptr_t lowest_string = (uintptr_t)(argc > 0 ? (void*)w.argv[0] : (void*)fn);
  CHECK(vec_end <= lowest_string, "vector end %#lx overlaps string block start %#lx",
        vec_end, lowest_string);

  printf("   argc=%ld envc=%d auxpairs=%d sp=%p (%zu bytes used of %zu)\n",
         w.argc, w.envc, w.npairs, sp, (size_t)(hi - (uintptr_t)sp), stksz);
  free(stack);
}

// ---------------------------------------------------------------------------
// enter_guest: real branch into a hand-written probe, in a forked child.
// The probe verifies the ABI and exits with argc, so the parent reads the result from wait().
// ---------------------------------------------------------------------------
extern void probe_entry(void);
__asm__(
    ".text\n"
    ".p2align 2\n"
    ".globl _probe_entry\n"
    "_probe_entry:\n"
    "  mov  x9, sp\n"
    "  and  x10, x9, #15\n"
    "  cbnz x10, 1f\n"          // SP must be 16-byte aligned
    "  cbnz x30, 2f\n"          // LR must be zero
    "  cbnz x29, 3f\n"          // FP must be zero
    "  cbnz x0, 4f\n"           // x0 must be zero
    "  ldr  x0, [sp]\n"         // argc -> exit status
    "  b    __exit\n"
    "1: mov x0, #101\n  b __exit\n"
    "2: mov x0, #102\n  b __exit\n"
    "3: mov x0, #103\n  b __exit\n"
    "4: mov x0, #104\n  b __exit\n");

static void test_enter_guest(void) {
  printf("-- case: enter_guest (forked probe)\n");
  static char* argv[] = { "/bin/busybox", "echo", "hi", "there", "you", 0 };
  static char* envp[] = { "PATH=/bin", 0 };
  struct prog_info p = { .base = 0x400000, .entry = 0x401000, .phdr = 0x400040,
                         .phnum = 9, .phent = 56, .interp_base = 0, .interp_entry = 0 };
  size_t stksz = 1u << 20;
  unsigned char* stack = (unsigned char*)malloc(stksz);
  void* sp = build_stack(stack, stksz, 5, argv, envp, &p);
  CHECK(sp != NULL, "build_stack returned NULL");
  if (!sp) { free(stack); return; }

  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) {
    enter_guest((unsigned long)&probe_entry, sp);   // never returns
  }
  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid, "waitpid failed");
  CHECK(WIFEXITED(st), "probe did not exit normally (status %#x)", st);
  if (WIFEXITED(st)) {
    int c = WEXITSTATUS(st);
    CHECK(c == 5, "probe exit %d: %s", c,
          c == 101 ? "SP not 16-byte aligned" :
          c == 102 ? "x30 (LR) not zero" :
          c == 103 ? "x29 (FP) not zero" :
          c == 104 ? "x0 not zero" : "argc read through SP was wrong");
    if (c == 5) printf("   probe saw sp aligned, x29=x30=x0=0, *sp == argc == 5\n");
  }
  free(stack);
}

static void test_entry_point(void) {
  printf("-- case: entry_point\n");
  struct prog_info dyn = { .entry = 0x401000, .interp_entry = 0x7f0000 };
  struct prog_info sta = { .entry = 0x401000, .interp_entry = 0 };
  CHECK(entry_point(&dyn) == 0x7f0000, "dynamic: want the interpreter entry");
  CHECK(entry_point(&sta) == 0x401000, "static: want the program entry");
}

int main(void) {
  printf("== bootstrap: Linux aarch64 initial stack ==\n");

  static char* argv1[] = { "/bin/busybox", "echo", "hello-from-linux", 0 };
  static char* envp1[] = { "PATH=/bin:/usr/bin", "TERM=xterm", "HOME=/root", "DISPLAY=:0",
                           "LIBGL_ALWAYS_INDIRECT=1", 0 };
  struct prog_info dyn = { .base = 0x100000000UL, .entry = 0x1000012a4UL, .phdr = 0x100000040UL,
                           .phnum = 11, .phent = 56,
                           .interp_base = 0x180000000UL, .interp_entry = 0x180000abcUL };
  run_case("dynamic (interp)", 3, argv1, envp1, &dyn, 1u << 20);

  struct prog_info sta = { .base = 0x400000, .entry = 0x400500, .phdr = 0x400040,
                           .phnum = 4, .phent = 56, .interp_base = 0, .interp_entry = 0 };
  static char* envp0[] = { 0 };
  run_case("static, no env", 1, argv1, envp0, &sta, 1u << 20);

  // argc == 0: AT_EXECFN must still be a valid empty string.
  run_case("no argv, no env", 0, NULL, envp0, &sta, 1u << 20);

  // Heap path: more argv+envp entries than the inline pointer index holds (INLINE_PTRS == 64).
  enum { BIGA = 300, BIGE = 200 };
  static char  abuf[BIGA][32], ebuf[BIGE][48];
  static char* abig[BIGA + 1];
  static char* ebig[BIGE + 1];
  for (int i = 0; i < BIGA; i++) { snprintf(abuf[i], sizeof abuf[i], "arg%03d-%*s", i, i % 9, "x");
                                   abig[i] = abuf[i]; }
  for (int i = 0; i < BIGE; i++) { snprintf(ebuf[i], sizeof ebuf[i], "VAR%03d=value-%d", i, i * 7);
                                   ebig[i] = ebuf[i]; }
  abig[BIGA] = 0; ebig[BIGE] = 0;
  run_case("large argv/envp (heap index)", BIGA, abig, ebig, &dyn, 1u << 20);

  // Tight region: must still produce a valid stack, or cleanly refuse.
  run_case("small region", 3, argv1, envp1, &dyn, 8192);

  // Region far too small for the strings: must return NULL, not scribble.
  {
    printf("-- case: region too small -> NULL\n");
    unsigned char* s = (unsigned char*)malloc(4096);
    void* sp = build_stack(s, 4096, BIGA, abig, ebig, &dyn);
    CHECK(sp == NULL, "expected NULL for an undersized region, got %p", sp);
    free(s);
  }

  test_entry_point();
  test_enter_guest();

  printf("\n%d checks, %d failures -> %s\n", checks, failures, failures ? "FAIL" : "PASS");
  return failures != 0;
}
