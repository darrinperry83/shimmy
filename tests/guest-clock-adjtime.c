#include <stdint.h>

struct lx_timex_clock {
  uint32_t modes, pad0;
  int64_t offset, freq, maxerror, esterror;
  int32_t status, pad1;
  int64_t constant, precision, tolerance;
  int64_t time_sec, time_usec, tick;
  int64_t ppsfreq, jitter;
  int32_t shift, pad2;
  int64_t stabil, jitcnt, calcnt, errcnt, stbcnt;
  int32_t tai;
  uint32_t reserved[11];
};
_Static_assert(sizeof(struct lx_timex_clock) == 208, "Linux timex size");

static long call(long nr, long a0, long a1, long a2) {
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}

static void done(int code) {
  call(93, code, 0, 0);
  for (;;) {}
}

void _start(void) {
  static struct lx_timex_clock tx;
  if (call(266, 0, 0, 0) != -14 || call(266, 0, 1, 0) != -14) done(1);
  if (call(266, 1, (long)&tx, 0) != -22) done(2);
  tx.modes = 1;
  tx.offset = 123456;
  if (call(266, 0, (long)&tx, 0) != -95 || tx.offset != 123456) done(3);
  tx.modes = 0;
  long state = call(266, 0, (long)&tx, 0);
  if (state < 0 || state > 5 || tx.time_sec < 1700000000 || tx.precision <= 0) done(4);
  static const char ok[] = "GUEST_CLOCK_ADJTIME_OK\n";
  call(64, 1, (long)ok, sizeof ok - 1);
  done(0);
}
