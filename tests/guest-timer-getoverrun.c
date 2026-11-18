// Raw ARM64 probe for timer_getoverrun on explicit SIGEV_NONE timers.
#include <stdint.h>

struct ts { int64_t sec, nsec; };
struct its { struct ts interval, value; };
struct lx_sigevent {
  uint64_t value;
  int32_t signo, notify;
  unsigned char rest[48];
};
static struct lx_sigevent ev = {.notify = 1};

static long sc1(long nr, long a) {
  register long x0 __asm__("x0") = a, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
  return x0;
}
static long sc2(long nr, long a, long b) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
  return x0;
}
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void finish(int status) {
  static const char ok[] = "timer_getoverrun staged ABI PASS\n";
  if (!status) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc1(93, status);
  for (;;) {}
}

void _start(void) {
  int32_t id = -1, real = -1;
  if (sc1(109, 0) != -22) finish(1);
  if (sc3(107, 1, (long)&ev, (long)&id) || id < 0) finish(2);
  if (sc3(107, 0, (long)&ev, (long)&real) || real < 0) finish(3);
  if (sc1(109, id) || sc1(109, real)) finish(4);

  struct its periodic = {.interval = {0, 2000000}, .value = {0, 1000000}};
  if (sc4(110, id, 0, (long)&periodic, 0)) finish(5);
  struct ts pause = {0, 25000000};
  if (sc2(101, (long)&pause, 0)) finish(6);
  struct its state;
  if (sc2(108, id, (long)&state) || sc1(109, id)) finish(7);
  // Many expirations do not create an overrun without signal delivery.
  if (sc4(110, real, 0, (long)&periodic, 0)) finish(8);
  if (sc2(101, (long)&pause, 0) || sc1(109, real)) finish(9);

  if (sc1(111, id) || sc1(109, id) != -22) finish(10);
  if (sc1(111, real) || sc1(109, real) != -22) finish(11);
  finish(0);
}
