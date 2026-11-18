// Raw ARM64 probe for a single virtual NUMA node and checked node masks.
#include <stdint.h>

static long sc5(long nr, long a, long b, long c, long d, long e) {
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void finish(int code) {
  static const char ok[] = "GUEST_MIGRATE_PAGES_OK\n";
  if (!code) sc5(64, 1, (long)ok, sizeof ok - 1, 0, 0);
  sc5(93, code, 0, 0, 0, 0);
  for (;;) {}
}
void _start(void) {
  uint64_t zero = 0, node0 = 1, node1 = 2;
  long self = sc5(172, 0, 0, 0, 0, 0);
  if (self <= 0) finish(1);
  if (sc5(238, 0, 2, (long)&node0, (long)&node0, 0) != 0) finish(2);
  if (sc5(238, self, 2, (long)&node0, (long)&zero, 0) != 0) finish(3);
  if (sc5(238, self, 2, (long)&zero, (long)&node0, 0) != 0) finish(4);
  if (sc5(238, self, 0, 0, 0, 0) != 0) finish(5);
  if (sc5(238, self, 1, 1, 1, 0) != 0) finish(6); // masks ignored at maxnode=1
  if (sc5(238, self, 0, (long)&zero, 0, 0) != -22) finish(7);
  if (sc5(238, self, 2, 1, (long)&node0, 0) != -14) finish(8);
  if (sc5(238, self, 3, (long)&node1, (long)&node0, 0) != -95) finish(9);
  if (sc5(238, self, 66, 0, 0, 0) != -95) finish(10);
  if (sc5(238, self, 32770, 0, 0, 0) != -22) finish(11);
  if (sc5(238, 2147483647, 2, (long)&node0, (long)&node0, 0) != -3) finish(12);
  finish(0);
}
