/* AArch64 guest probe for checked setxattr inputs. */
static long sc5(long nr, long a, long b, long c, long d, long e) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_SETXATTR_INPUT_OK\n";
  if (!code) sc5(64, 1, (long)ok, sizeof ok - 1, 0, 0);
  sc5(93, code, 0, 0, 0, 0);
  for (;;) {}
}
void _start(void) {
  static const char path[] = "/tmp/xattr-file";
  static const char name[] = "user.shimmy";
  static const char first[] = "value", second[] = "renewed";
  if (sc5(5, 0, (long)name, (long)first, sizeof first - 1, 1) != -14) done(1);
  if (sc5(5, 1, (long)name, (long)first, sizeof first - 1, 1) != -14) done(2);
  if (sc5(5, (long)path, 0, (long)first, sizeof first - 1, 1) != -14) done(3);
  if (sc5(5, (long)path, 1, (long)first, sizeof first - 1, 1) != -14) done(4);
  if (sc5(5, (long)path, (long)name, 1, sizeof first - 1, 1) != -14) done(5);
  if (sc5(5, (long)path, (long)name, (long)first, 65537, 1) != -7) done(6);
  if (sc5(5, (long)path, (long)name, (long)first, sizeof first - 1, 1) != 0) done(7);
  if (sc5(5, (long)path, (long)name, (long)second, sizeof second - 1, 2) != 0) done(8);
  done(0);
}
