// libc-free ARM64 guest probe for the bounded MPOL_DEFAULT mbind contract.
#include <stdint.h>

static long call(long nr, long a, long b, long c, long d, long e, long f) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x5 __asm__("x5") = f;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                   "r"(x4), "r"(x5), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int status) {
  static const char ok[] = "GUEST_MBIND_DEFAULT_OK\n";
  if (!status) call(64, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  call(93, status, 0, 0, 0, 0, 0);
  for (;;) {}
}
static long mbind(long start, long len, long mode, long mask, long maxnode,
                  long flags) {
  return call(235, start, len, mode, mask, maxnode, flags);
}
void _start(void) {
  long page = call(222, 0, 32768, 3, 0x22, -1, 0);
  if (page < 0) done(1);
  *(volatile char*)page = 1;
  *(volatile char*)(page + 16384) = 2;
  if (mbind(page, 32768, 0, 0, 0, 0) != 0) done(2);
  if (mbind(page, 0, 0, 0, 9999, 0) != 0) done(3);
  if (mbind(0, 0, 0, 0, 0, 0) != 0) done(4);
  if (mbind(page + 1, 16384, 0, 0, 0, 0) != -22) done(5);
  if (mbind(page, 16384, 7, 0, 0, 0) != -22) done(6);
  if (mbind(page, 16384, (1 << 14) | (1 << 15), 0, 0, 0) != -22) done(7);
  if (mbind(page, 16384, 0, 0, 0, 8) != -22) done(8);
  if (mbind(page, 16384, 2, 0, 0, 0) != -95) done(9);
  uint64_t mask = 1;
  if (mbind(page, 16384, 0, (long)&mask, 1, 0) != -95) done(10);
  if (mbind(0x100000000ul, 16384, 0, 0, 0, 0) != -14) done(11);
  if (mbind(page, 16384, 0, 0, 0, 1) != -95) done(12);
  if (mbind(page, -16384L, 0, 0, 0, 0) != -22) done(13);
  if (call(215, page, 32768, 0, 0, 0, 0) != 0) done(14); // munmap
  if (mbind(page, 16384, 0, 0, 0, 0) != -14) done(15);
  done(0);
}
