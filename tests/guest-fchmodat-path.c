/* AArch64 guest probe for bounded fchmodat pathname copying. */
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_FCHMODAT_PATH_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
void _start(void) {
  static const char path[] = "/tmp/fchmodat-path-file";
  if (sc4(53, -100, 0, 0640, 0) != -14) done(1);
  if (sc4(53, -100, 1, 0640, 0) != -14) done(2);
  /* x3 is not an argument to the original three-argument Linux syscall. */
  if (sc4(53, -100, (long)path, 0640, 0x400) != 0) done(3);
  done(0);
}
