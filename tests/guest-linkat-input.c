/* AArch64 guest probe for checked linkat pathnames. */
static long sc5(long nr, long a, long b, long c, long d, long e) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_LINKAT_INPUT_OK\n";
  if (!code) sc5(64, 1, (long)ok, sizeof ok - 1, 0, 0);
  sc5(93, code, 0, 0, 0, 0);
  for (;;) {}
}
void _start(void) {
  static const char source[] = "/a/source", target[] = "/a/linked";
  static const char dir[] = "/a", rel_source[] = "source";
  static const char rel_target[] = "relative-linked";
  if (sc5(37, -100, 0, -100, (long)target, 0) != -14) done(1);
  if (sc5(37, -100, (long)source, -100, 0, 0) != -14) done(2);
  if (sc5(37, -100, (long)source, -100, 1, 0) != -14) done(3);
  if (sc5(37, -100, (long)source, -100, (long)target, 0) != 0) done(4);
  long dfd = sc5(56, -100, (long)dir, 65536, 0, 0); /* O_DIRECTORY */
  if (dfd < 0) done(5);
  if (sc5(37, dfd, (long)rel_source, dfd, (long)rel_target, 0) != 0) done(6);
  done(0);
}
