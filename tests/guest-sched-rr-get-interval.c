/* Raw arm64 guest probe for bounded sched_rr_get_interval behavior. */
#include <stdint.h>

struct timespec64 { int64_t sec, nsec; };
static long sc(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
__attribute__((noreturn)) static void done(int code) {
  static const char ok[] = "SCHED_RR_GET_INTERVAL_BOUNDED_OK\n";
  if (!code) sc(64, 1, (long)ok, sizeof ok - 1);
  sc(93, code, 0, 0);
  for (;;) {}
}
void _start(void) {
  long pid = sc(172, 0, 0, 0), tid = sc(178, 0, 0, 0);
  struct timespec64 interval = {123, 456};
  if (pid <= 0 || tid <= 0) done(1);
  if (sc(120, 0, 0, 0) != 0) done(2); /* SCHED_OTHER */
  if (sc(127, 0, (long)&interval, 0) != -95) done(3);
  if (interval.sec != 123 || interval.nsec != 456) done(4);
  if (sc(127, tid, (long)&interval, 0) != -95) done(5);
  if (sc(127, pid, (long)&interval, 0) != -95) done(6);
  if (sc(127, -1, (long)&interval, 0) != -22) done(7);
  if (sc(127, 2147483647, (long)&interval, 0) != -3) done(8);
  if (sc(127, 0, 0, 0) != -14) done(9);
  if (sc(127, 0, 0x30000, 0) != -14) done(10);
  done(0);
}
