// Raw ARM64 probe for the bounded sched_getscheduler PID contract.
#include <stdint.h>

static long sc1(long nr, long arg) {
  register long x0 __asm__("x0") = arg;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
  return x0;
}
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void finish(int code) {
  static const char ok[] = "GUEST_SCHED_GETSCHEDULER_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc1(93, code);
  for (;;) {}
}
void _start(void) {
  long pid = sc1(172, 0);
  long tid = sc1(178, 0);
  if (pid <= 0 || tid <= 0) finish(1);
  if (sc1(120, 0) != 0) finish(2);
  if (sc1(120, tid) != 0) finish(3);
  if (sc1(120, pid) != 0) finish(4);
  if (sc1(120, -1) != -22) finish(5);
  if (sc1(120, 2147483647) != -3) finish(6);
  finish(0);
}
