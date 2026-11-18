/* AArch64 guest probe for checked mount target/fstype strings. */
static long sc5(long nr, long a, long b, long c, long d, long e) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_MOUNT_INPUT_OK\n";
  if (!code) sc5(64, 1, (long)ok, sizeof ok - 1, 0, 0);
  sc5(93, code, 0, 0, 0, 0);
  for (;;) {}
}
void _start(void) {
  static const char target[] = "/tmp", fstype[] = "tmpfs";
  if (sc5(40, 0, 0, (long)fstype, 0, 0) != -14) done(1);
  if (sc5(40, 0, 1, (long)fstype, 0, 0) != -14) done(2);
  if (sc5(40, 0, (long)target, 1, 0, 0) != -14) done(3);
  if (sc5(40, 0, (long)target, (long)fstype, 0, 0) != 0) done(4);
  done(0);
}
