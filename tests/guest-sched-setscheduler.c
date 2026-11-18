#include <stdio.h>
#include <stdint.h>

static long call3(long nr, long a0, long a1, long a2) {
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}

int main(void) {
  int32_t zero = 0, one = 1;
  long tid = call3(178, 0, 0, 0);
  long pid = call3(172, 0, 0, 0);
  if (tid <= 0 || pid <= 0) return 1;
  if (call3(119, 0, 0, (long)&zero) != 0) return 2;
  if (call3(119, tid, 0, (long)&zero) != 0) return 3;
  if (call3(119, pid, 0, (long)&zero) != 0) return 4;
  if (call3(119, -1, 0, (long)&zero) != -22) return 5;
  if (call3(119, 999999, 0, (long)&zero) != -3) return 6;
  if (call3(119, 0, 0, 0) != -22) return 7;
  if (call3(119, 0, 0, 1) != -14) return 8;
  if (call3(119, 0, 0, (long)&one) != -22) return 9;
  if (call3(119, 0, 1, (long)&one) != -95) return 10;
  if (call3(119, 0, 3, (long)&zero) != -95) return 11;
  if (call3(119, 0, 0x40000000, (long)&zero) != -95) return 12;
  if (call3(119, 0, 4, (long)&zero) != -22) return 13;
  puts("GUEST_SCHED_SETSCHEDULER_OK");
  return 0;
}
