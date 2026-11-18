#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

struct linux_timespec { int64_t sec, nsec; };
struct sigarg { const uint64_t *mask; uint64_t size; };

static long raw_pselect6(long n, void *r, void *w, void *e, void *timeout,
                         void *sigarg) {
  register long x0 __asm__("x0") = n;
  register long x1 __asm__("x1") = (long)r;
  register long x2 __asm__("x2") = (long)w;
  register long x3 __asm__("x3") = (long)e;
  register long x4 __asm__("x4") = (long)timeout;
  register long x5 __asm__("x5") = (long)sigarg;
  register long x8 __asm__("x8") = 72;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                   "r"(x4), "r"(x5), "r"(x8) : "memory");
  return x0;
}

int main(void) {
  int p[2];
  if (pipe(p)) return 1;
  struct linux_timespec zero = {0, 0};
  uint64_t readset[16] = {0};
  readset[p[0] / 64] |= 1ULL << (p[0] % 64);
  if (raw_pselect6(p[0] + 1, readset, 0, 0, &zero, 0) != 0) return 2;
  if (readset[p[0] / 64] != 0) return 3;
  if (write(p[1], "x", 1) != 1) return 4;
  readset[p[0] / 64] |= 1ULL << (p[0] % 64);
  if (raw_pselect6(p[0] + 1, readset, 0, 0, &zero, 0) != 1) return 5;
  if (!(readset[p[0] / 64] & (1ULL << (p[0] % 64)))) return 6;

  uint64_t mask = 0;
  struct sigarg arg = {&mask, 8};
  if (raw_pselect6(0, 0, 0, 0, &zero, &arg) != -95) return 7;
  arg.size = 4;
  if (raw_pselect6(0, 0, 0, 0, &zero, &arg) != -22) return 8;
  arg.size = 8;
  arg.mask = (void*)1;
  if (raw_pselect6(0, 0, 0, 0, &zero, &arg) != -14) return 9;
  if (raw_pselect6(0, 0, 0, 0, &zero, (void*)1) != -14) return 10;
  arg.mask = 0; arg.size = 123;
  if (raw_pselect6(0, 0, 0, 0, &zero, &arg) != 0) return 11;
  if (raw_pselect6(0, (void*)1, 0, 0, &zero, 0) != 0) return 12;
  if (raw_pselect6(1, (void*)1, 0, 0, &zero, 0) != -14) return 13;
  struct linux_timespec invalid = {0, 1000000000};
  if (raw_pselect6(0, 0, 0, 0, &invalid, 0) != -22) return 14;
  if (raw_pselect6(1025, 0, 0, 0, &zero, 0) != -95) return 15;
  struct linux_timespec brief = {0, 10000000};
  if (raw_pselect6(0, 0, 0, 0, &brief, 0) != 0 || brief.sec != 0 ||
      brief.nsec != 0) return 16;
  close(p[0]); close(p[1]);
  puts("GUEST_PSELECT6_OK");
  return 0;
}
