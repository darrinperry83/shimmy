// Raw AArch64 probe of the bounded virtual CPU affinity query.
#include <stdint.h>

static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void finish(int code) {
  static const char ok[] = "GUEST_SCHED_GETAFFINITY_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
void _start(void) {
  long pid = sc3(172, 0, 0, 0);
  long tid = sc3(178, 0, 0, 0);
  if (pid <= 0 || tid <= 0) finish(1);
  uint64_t mask[2] = {0, UINT64_C(0x123456789abcdef0)};
  if (sc3(123, 0, sizeof mask, (long)mask) != 8) finish(2);
  if (!mask[0] || mask[1] != UINT64_C(0x123456789abcdef0)) finish(3);
  uint64_t expected = mask[0];
  mask[0] = 0;
  if (sc3(123, tid, 8, (long)mask) != 8 || mask[0] != expected) finish(4);
  mask[0] = 0;
  if (sc3(123, pid, 16, (long)mask) != 8 || mask[0] != expected) finish(5);
  if (sc3(123, 0, 4, (long)mask) != -22) finish(6);
  if (sc3(123, 0, 9, (long)mask) != -22) finish(7);
  if (sc3(123, 0, 8, 0) != -14) finish(8);
  if (sc3(123, 0, 8, 1) != -14) finish(9);
  if (sc3(123, 2147483647, 8, (long)mask) != -3) finish(10);
  finish(0);
}
