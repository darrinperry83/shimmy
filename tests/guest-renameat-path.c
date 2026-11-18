/* AArch64 guest probe for checked renameat pathnames. */
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_RENAMEAT_PATH_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
void _start(void) {
  static const char source[] = "/a/source", target[] = "/a/target";
  static const char dir[] = "/a", rel_source[] = "relative-source";
  static const char rel_target[] = "relative-target";
  if (sc4(38, -100, 0, -100, (long)target) != -14) done(1);
  if (sc4(38, -100, (long)source, -100, 0) != -14) done(2);
  if (sc4(38, -100, (long)source, -100, 1) != -14) done(3);
  if (sc4(38, -100, (long)source, -100, (long)target) != 0) done(4);
  long dfd = sc4(56, -100, (long)dir, 65536, 0); /* O_DIRECTORY */
  if (dfd < 0) done(5);
  if (sc4(38, dfd, (long)rel_source, dfd, (long)rel_target) != 0) done(6);
  done(0);
}
