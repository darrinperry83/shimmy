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

static long call(long nr, long arg) {
  register long x0 __asm__("x0") = arg;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
  return x0;
}

static void done(int code) {
  call(93, code);
  for (;;) {}
}

void _start(void) {
  static struct lx_timex_clock tx;
  if (call(171, 0) != -14) done(1);
  if (call(171, 1) != -14) done(2);

  tx.modes = 1;
  tx.offset = 123456;
  if (call(171, (long)&tx) != -95 || tx.offset != 123456) done(3);

  tx.modes = 0;
  long state = call(171, (long)&tx);
  if (state < 0 || state > 5) done(4);
  if (tx.time_sec < 1700000000 || tx.time_usec < 0 ||
      tx.time_usec >= ((tx.status & 0x2000) ? 1000000000 : 1000000)) done(5);
  if (tx.tick != 10000 || tx.precision <= 0 || tx.tolerance <= 0) done(6);

  static const char ok[] = "GUEST_ADJTIMEX_QUERY_OK\n";
  register long x0 __asm__("x0") = 1;
  register long x1 __asm__("x1") = (long)ok;
  register long x2 __asm__("x2") = sizeof ok - 1;
  register long x8 __asm__("x8") = 64;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  done(0);
}
