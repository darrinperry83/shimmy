/* AArch64 guest probe for a disposable file and shim-owned timerfd. */
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_FCHMOD_RESERVED_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
void _start(void) {
  static const char path[] = "/tmp/fchmod-file";
  long fd = sc4(56, -100, (long)path, 0, 0);  /* openat read-only */
  if (fd < 0) done(1);
  if (sc4(52, fd, 0640, 0, 0) != 0) done(2);
  if (sc4(52, -1, 0600, 0, 0) != -9) done(3);
  long timerfd = sc4(85, 1, 0, 0, 0);         /* CLOCK_MONOTONIC */
  if (timerfd < 0) done(4);
  if (sc4(52, timerfd, 0600, 0, 0) != -9) done(5);
  done(0);
}
