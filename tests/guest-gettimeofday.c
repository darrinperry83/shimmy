/* AArch64 gettimeofday: Linux timeval layout and checked optional outputs. */
#include <stdint.h>

static long sc(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}

static __attribute__((noreturn)) void finish(int code) {
  static const char ok[] = "GUEST_GETTIMEOFDAY_OK\n";
  if (!code) sc(64, 1, (long)ok, sizeof ok - 1);
  sc(93, code, 0, 0);
  for (;;) {}
}

void _start(void) {
  struct { int64_t sec, usec; } tv = {-1, -1};
  int32_t tz[2] = {-1, -1};
  if (sc(169, (long)&tv, (long)tz, 0) != 0) finish(1);
  if (tv.sec < 1500000000 || tv.usec < 0 || tv.usec >= 1000000) finish(2);
  if (tz[0] != 0 || tz[1] != 0) finish(3);
  if (sc(169, 0, 0, 0) != 0) finish(4);
  if (sc(169, 1, 0, 0) != -14) finish(5);
  if (sc(169, 0, 1, 0) != -14) finish(6);
  tz[0] = tz[1] = -1;
  if (sc(169, 0, (long)tz, 0) != 0 || tz[0] || tz[1]) finish(7);
  finish(0);
}
