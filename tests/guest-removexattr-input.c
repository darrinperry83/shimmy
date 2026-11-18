/* AArch64 guest probe for checked removexattr pathname and name. */
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_REMOVEXATTR_INPUT_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
void _start(void) {
  static const char path[] = "/tmp/xattr-file";
  static const char name[] = "user.shimmy";
  if (sc3(14, 0, (long)name, 0) != -14) done(1);
  if (sc3(14, 1, (long)name, 0) != -14) done(2);
  if (sc3(14, (long)path, 0, 0) != -14) done(3);
  if (sc3(14, (long)path, 1, 0) != -14) done(4);
  if (sc3(14, (long)path, (long)name, 0) != 0) done(5);
  done(0);
}
