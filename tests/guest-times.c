/* Linux AArch64 times(2): optional tms, monotonic ticks and bad output. */
#include <stdint.h>
struct tms { int64_t utime, stime, cutime, cstime; };
static long sc(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void finish(int status) {
  static const char ok[] = "GUEST_TIMES_OK\n";
  if (!status) sc(64, 1, (long)ok, sizeof ok - 1);
  sc(93, status, 0, 0);
  for (;;) {}
}
void _start(void) {
  struct tms t = {-1, -1, -1, -1};
  long first = sc(153, (long)&t, 0, 0);
  if (first < 0 || t.utime < 0 || t.stime < 0 ||
      t.cutime < 0 || t.cstime < 0) finish(1);
  long second = sc(153, 0, 0, 0);
  if (second < first) finish(2);
  if (sc(153, 1, 0, 0) != -14) finish(3);
  struct tms after = {-1, -1, -1, -1};
  if (sc(153, (long)&after, 0, 0) < second ||
      after.utime < t.utime || after.stime < t.stime) finish(4);
  finish(0);
}
