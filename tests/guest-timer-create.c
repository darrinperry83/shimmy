// Raw ARM64 syscall 107 probe for the staged SIGEV_NONE timer registry.
#include <stdint.h>

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
_Static_assert(sizeof(struct lx_sigevent) == 64, "Linux ARM64 sigevent ABI");
static struct lx_sigevent ev = {.notify = 1};

static __attribute__((noreturn)) void finish(int status) {
  static const char ok[] = "timer_create staged ABI PASS\n";
  if (!status) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, status, 0, 0);
  for (;;) {}
}

void _start(void) {
  int32_t a = -1, b = -1;
  if (sc3(107, 7, (long)&ev, (long)&a) != -22) finish(1);
  if (sc3(107, 0, 0, (long)&a) != -95) finish(2);
  if (sc3(107, 1, 0x100000, (long)&a) != -14) finish(3);
  if (sc3(107, 1, (long)&ev, 0x100000) != -14) finish(4);
  ev.notify = 9;
  if (sc3(107, 1, (long)&ev, (long)&a) != -22) finish(5);
  ev.notify = 0;
  if (sc3(107, 1, (long)&ev, (long)&a) != -95) finish(6);
  ev.notify = 1;
  if (sc3(107, 0, (long)&ev, (long)&a) != 0 || a != 0) finish(7);
  if (sc3(107, 1, (long)&ev, (long)&b) != 0 || b != 1) finish(8);
  long child = sc5(220, 17, 0, 0, 0, 0); // clone(SIGCHLD), no CLONE_VM
  if (child == 0) {
    int32_t child_id = -1;
    if (sc3(107, 1, (long)&ev, (long)&child_id) != 0 || child_id != 0)
      sc3(93, 9, 0, 0);
    sc3(93, 0, 0, 0);
    for (;;) {}
  }
  if (child < 0) finish(10);
  int status = -1;
  if (sc5(260, child, (long)&status, 0, 0, 0) != child || status != 0)
    finish(11);
  int32_t parent_id = -1;
  if (sc3(107, 1, (long)&ev, (long)&parent_id) != 0 || parent_id != 2)
    finish(12);
  finish(0);
}
