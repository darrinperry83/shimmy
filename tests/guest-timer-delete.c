// Raw ARM64 create/delete probe for the staged SIGEV_NONE timer registry.
#include <stdint.h>

static long sc1(long nr, long a) {
  register long x0 __asm__("x0") = a, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
  return x0;
}
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static long sc5(long nr, long a, long b, long c, long d, long e) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                   "r"(x4), "r"(x8) : "memory");
  return x0;
}

struct lx_sigevent {
  uint64_t value;
  int32_t signo;
  int32_t notify;
  unsigned char rest[48];
};
static struct lx_sigevent ev = {.notify = 1};

static __attribute__((noreturn)) void finish(int status) {
  static const char ok[] = "timer_delete staged ABI PASS\n";
  if (!status) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc1(93, status);
  for (;;) {}
}

void _start(void) {
  int32_t id = -1, next = -1;
  if (sc1(111, -1) != -22) finish(1);
  if (sc3(107, 1, (long)&ev, (long)&id) != 0 || id != 0) finish(2);
  long child = sc5(220, 17, 0, 0, 0, 0); // clone(SIGCHLD), separate guest mm
  if (child == 0) {
    sc1(93, sc1(111, id) == -22 ? 0 : 8);
    for (;;) {}
  }
  if (child < 0) finish(9);
  int status = -1;
  if (sc5(260, child, (long)&status, 0, 0, 0) != child || status != 0)
    finish(10);
  if (sc1(111, id) != 0) finish(3);
  if (sc1(111, id) != -22) finish(4);
  if (sc3(107, 0, (long)&ev, (long)&next) != 0 || next == id) finish(5);
  if (sc1(111, next) != 0) finish(6);
  if (sc1(111, 0x7fffffff) != -22) finish(7);
  finish(0);
}
