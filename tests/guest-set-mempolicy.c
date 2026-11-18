#include <stdint.h>
#include <stdio.h>

static long raw_set_mempolicy(long mode, const unsigned long* mask,
                              unsigned long maxnode) {
  register long x0 __asm__("x0") = mode;
  register long x1 __asm__("x1") = (long)mask;
  register long x2 __asm__("x2") = (long)maxnode;
  register long x8 __asm__("x8") = 237;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}

int main(void) {
  unsigned long empty = 0;
  unsigned long node0 = 1;
  unsigned long ignored_high = 2;
  if (raw_set_mempolicy(0, 0, 0) != 0) return 1;
  if (raw_set_mempolicy(0, 0, 123) != 0) return 2;
  if (raw_set_mempolicy(0, &empty, 2) != 0) return 3;
  if (raw_set_mempolicy(0, &node0, 2) != -22) return 4;
  if (raw_set_mempolicy(0, &ignored_high, 2) != 0) return 5;
  if (raw_set_mempolicy(0, (void*)1, 1) != 0) return 6;
  if (raw_set_mempolicy(0, (void*)1, 2) != -14) return 7;
  if (raw_set_mempolicy(0, &empty, 0) != -22) return 8;
  if (raw_set_mempolicy(1, &node0, 2) != -95) return 9;
  if (raw_set_mempolicy(2, &node0, 2) != -95) return 10;
  if (raw_set_mempolicy(7, 0, 0) != -22) return 11;
  if (raw_set_mempolicy(0xc000, 0, 0) != -22) return 12;
  if (raw_set_mempolicy(0x2000, 0, 0) != -22) return 13;
  if (raw_set_mempolicy(0, &empty, 66) != -95) return 14;
  puts("GUEST_SET_MEMPOLICY_OK");
  return 0;
}
