// Raw ARM64 probe for the bounded sched_getparam contract.
#include <stdint.h>

static long sc1(long nr, long arg) {
  register long x0 __asm__("x0") = arg;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
  return x0;
}
static long sc2(long nr, long a, long b) {
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
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
  static const char ok[] = "GUEST_SCHED_GETPARAM_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc1(93, code);
  for (;;) {}
}
void _start(void) {
  long pid = sc1(172, 0);
  long tid = sc1(178, 0);
  int32_t priority = 123;
  if (pid <= 0 || tid <= 0) finish(1);
  if (sc2(121, 0, (long)&priority) != 0 || priority != 0) finish(2);
  priority = 123;
  if (sc2(121, tid, (long)&priority) != 0 || priority != 0) finish(3);
  priority = 123;
  if (sc2(121, pid, (long)&priority) != 0 || priority != 0) finish(4);
  if (sc2(121, -1, (long)&priority) != -22) finish(5);
  if (sc2(121, 0, 0) != -22) finish(6);
  if (sc2(121, 0, 1) != -14) finish(7);
  priority = 123;
  if (sc2(121, 2147483647, (long)&priority) != -3 || priority != 123) finish(8);
  finish(0);
}
