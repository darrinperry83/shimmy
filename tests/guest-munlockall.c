// Raw ARM64 guest probe: lock a guest mapping, unlock all, then clean up.
#include <stdint.h>

static long sc6(long nr, long a, long b, long c, long d, long e, long f) {
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e;
  register long x5 __asm__("x5") = f;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void finish(int code) {
  static const char ok[] = "GUEST_MUNLOCKALL_UNSUPPORTED_OK\n";
  if (!code) sc6(64, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  sc6(93, code, 0, 0, 0, 0, 0);
  for (;;) {}
}
void _start(void) {
  const long page = 16384;
  long addr = sc6(222, 0, page, 3, 0x22, -1, 0); // private anonymous mmap
  if (addr < 0) finish(1);
  if (sc6(228, addr, page, 0, 0, 0, 0) != 0) finish(2); // mlock
  if (sc6(231, 0, 0, 0, 0, 0, 0) != -38) finish(3); // ENOSYS, no false unlock
  if (sc6(231, 0, 0, 0, 0, 0, 0) != -38) finish(4);
  if (sc6(229, addr, page, 0, 0, 0, 0) != 0) finish(5);
  if (sc6(215, addr, page, 0, 0, 0, 0) != 0) finish(6);
  finish(0);
}
