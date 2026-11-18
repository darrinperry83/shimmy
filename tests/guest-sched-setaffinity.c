#include <stdio.h>
#include <string.h>

static long call3(long nr, long a0, long a1, long a2) {
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}

int main(void) {
  unsigned char mask[128], restricted[128], zero[128] = {0};
  long tid = call3(178, 0, 0, 0), pid = call3(172, 0, 0, 0);
  if (tid <= 0 || pid <= 0) return 1;
  memset(mask, 0, sizeof mask);
  if (call3(123, 0, sizeof mask, (long)mask) <= 0) return 2;
  if (call3(122, 0, sizeof mask, (long)mask) != 0) return 3;
  if (call3(122, tid, sizeof mask, (long)mask) != 0) return 4;
  if (call3(122, pid, sizeof mask, (long)mask) != 0) return 5;
  if (call3(122, 999999, sizeof mask, (long)mask) != -3) return 6;
  if (call3(122, 0, sizeof mask, 0) != -14) return 7;
  if (call3(122, 0, sizeof mask, 1) != -14) return 8;
  if (call3(122, 0, 4, (long)mask) != -22) return 9;
  if (call3(122, 0, 129, (long)mask) != -95) return 10;
  memcpy(restricted, mask, sizeof mask);
  restricted[0] &= ~1u;
  if (call3(122, 0, sizeof restricted, (long)restricted) != -95) return 11;
  if (call3(122, 0, sizeof zero, (long)zero) != -95) return 12;
  puts("GUEST_SCHED_SETAFFINITY_OK");
  return 0;
}
