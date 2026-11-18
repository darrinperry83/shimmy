/* AArch64 getrusage selector and checked 144-byte Linux output probe. */
#include <stdint.h>

static long sc(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}

struct lx_timeval { int64_t sec, usec; };
struct lx_rusage {
  struct lx_timeval utime, stime;
  int64_t values[14];
};
_Static_assert(sizeof(struct lx_rusage) == 144, "AArch64 Linux rusage size");

static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_GETRUSAGE_OK\n";
  if (!code) sc(64, 1, (long)ok, sizeof ok - 1);
  sc(93, code, 0, 0);
  for (;;) {}
}

void _start(void) {
  struct lx_rusage self, children;
  if (sc(165, 0, (long)&self, 0) != 0) done(1);
  if (self.utime.sec < 0 || self.stime.sec < 0 ||
      self.utime.usec < 0 || self.utime.usec >= 1000000 ||
      self.stime.usec < 0 || self.stime.usec >= 1000000 ||
      self.values[0] < 0) done(2);
  if (sc(165, -1, (long)&children, 0) != 0) done(3);
  if (children.utime.sec < 0 || children.stime.sec < 0) done(4);
  if (sc(165, 1, (long)&self, 0) != -95) done(5);  /* unsupported THREAD */
  if (sc(165, 2, (long)&self, 0) != -22) done(6);  /* invalid selector */
  if (sc(165, 0, 0, 0) != -14) done(7);
  if (sc(165, 0, 1, 0) != -14) done(8);
  done(0);
}
